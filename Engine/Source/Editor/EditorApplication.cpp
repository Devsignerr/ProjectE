#include "Editor/EditorApplication.h"

#include "Audio/AudioReflection.h"
#include "Physics/PhysicsReflection.h"
#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Editor/EditorActions.h"
#include "Editor/EditorCameraState.h"
#include "Editor/EditorTheme.h"
#include "Editor/SceneEditOps.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/ModelImportSettings.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Particles.h"
#include "Scene/SceneSerializer.h"

#include <commdlg.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (기본 레이아웃)

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	FApplicationDesc MakeEditorDesc()
	{
		FApplicationDesc Desc;
		Desc.Window.Title  = L"ProjectE Editor";
		Desc.Window.Width  = 1600;
		Desc.Window.Height = 900;
		return Desc;
	}

	// Win32 파일 대화상자. 취소하면 빈 경로
	std::filesystem::path ShowSceneFileDialog(HWND Owner, const std::filesystem::path& InitialDirectory, bool bSave)
	{
		wchar_t Buffer[MAX_PATH] = {};
		const std::wstring InitialDir = InitialDirectory.wstring();

		OPENFILENAMEW Dialog{};
		Dialog.lStructSize     = sizeof(Dialog);
		Dialog.hwndOwner       = Owner;
		Dialog.lpstrFilter     = L"ProjectE 씬 (*.escene)\0*.escene\0모든 파일 (*.*)\0*.*\0";
		Dialog.lpstrFile       = Buffer;
		Dialog.nMaxFile        = MAX_PATH;
		Dialog.lpstrInitialDir = InitialDir.c_str();
		Dialog.lpstrDefExt     = L"escene";
		Dialog.Flags           = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (bSave ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

		const BOOL bOk = bSave ? GetSaveFileNameW(&Dialog) : GetOpenFileNameW(&Dialog);
		return bOk ? std::filesystem::path(Buffer) : std::filesystem::path();
	}
} // namespace

FEditorApplication::FEditorApplication()
	: FApplication(MakeEditorDesc())
{
}

FEditorApplication::~FEditorApplication() = default;

bool FEditorApplication::OnInit()
{
	RegisterAudioTypes(); // 씬 로드 전에 (인스펙터/직렬화)
	RegisterPhysicsTypes();
	// 게임 모듈 (.eproject "GameModule"): 씬 로드 전에 게임 컴포넌트 타입을 등록한다
	if (FPaths::HasProject() && !FPaths::GetProjectDescriptor().GameModule.empty())
	{
		GameModule.Load(FGameModuleHost::GetDefaultModulePath(FPaths::GetProjectDescriptor().GameModule));
	}

	FD3D12RHIDesc RhiDesc;
	RhiDesc.WindowHandle = GetWindow().GetHandle();
	RhiDesc.Width        = GetWindow().GetWidth();
	RhiDesc.Height       = GetWindow().GetHeight();
#if E_DEBUG
	RhiDesc.bEnableDebugLayer = true;
#endif

	Rhi = std::make_unique<FD3D12RHI>();
	if (!Rhi->Init(RhiDesc))
	{
		return false;
	}
	if (!Resources.Init(*Rhi))
	{
		return false;
	}
	if (!SceneRenderer.Init(*Rhi, Resources))
	{
		return false;
	}
	if (!ImGuiLayer.Init(GetWindow(), *Rhi, FPaths::GetSavedDirectory() / L"EditorLayout.ini"))
	{
		return false;
	}

	Context.Rhi              = Rhi.get();
	Context.Resources        = &Resources;
	Context.Renderer         = &SceneRenderer;
	Context.Scene            = &Scene;
	Context.Camera           = &Camera;
	Context.ContentDirectory = FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory();
	Context.DefaultCubeMesh  = Resources.GetOrCreatePrimitiveMesh("cube");
	Context.OpenSceneRequest = [this](const std::filesystem::path& Path) { OpenScene(Path); };
	Context.OpenAssetEditorRequest = [this](const std::filesystem::path& Path) {
		if (!AssetEditors.Open(Context, Path))
		{
			ShowNotification("에셋을 열지 못했습니다: " + FStringConv::ToUtf8(Path.filename().wstring()), true);
		}
	};
	Context.PrepareAssetChange = [this](const std::vector<std::filesystem::path>& Paths) { return AssetEditors.CloseEditorsFor(Context, Paths); };
	Context.AssetsMoved        = [this](const std::vector<FAssetMove>& Moves) { OnAssetsMoved(Moves); };
	Context.Notify             = [this](const std::string& Message, bool bError) { ShowNotification(Message, bError); };
	Context.ReimportModel      = [this](const std::filesystem::path& Path) { return ReimportModelAsset(Path); };
	Context.Scripts          = &Scripts;
	Scripts.SetContentDirectory(Context.ContentDirectory);
	Scripts.SetAudioHooks({
		[this](FEntity Entity) { AudioSystem.Play(Audio, Entity); },
		[this](FEntity Entity) { AudioSystem.Stop(Audio, Entity); },
		[this](const std::string& ClipAsset) { Audio.PlayOneShot(Scripts.GetContentDirectory() / FStringConv::ToWide(ClipAsset)); },
	});
	if (Audio.Init() && IsAutomationRun())
	{
		Audio.SetMasterVolume(0.0f); // 자동 검증 중에는 소리를 내지 않는다
	}
	Scripts.SetPhysicsHooks({
		[this](const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics.Raycast(Origin, Direction, MaxDistance, Hit))
			{
				return false;
			}
			OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance };
			return true;
		},
		[this](FEntity Entity, const FVector3& Force) { Physics.AddForce(Entity, Force); },
		[this](FEntity Entity, const FVector3& Impulse) { Physics.AddImpulse(Entity, Impulse); },
		[this](FEntity Entity, const FVector3& Velocity) { Physics.SetVelocity(Entity, Velocity); },
		[this](FEntity Entity) { return Physics.GetVelocity(Entity); },
	});
	PlayMode.Init(Scene, Scripts, &Physics);
	PlayMode.SetGameModule(&GameModule);

	// --scene <Content 기준 경로>: 시작 씬 지정 (데모/자동 검증). 없거나 실패하면 프로젝트 기본 씬
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	if (const std::wstring SceneArg = CommandLine.GetValue(L"--scene"); SceneArg.empty() || !OpenScene(Context.ContentDirectory / SceneArg))
	{
		OpenStartupScene();
	}

	// 자동 검증: --select <이름>[,<이름>...] 으로 시작 시 엔티티 선택 (선택 아웃라인/인스펙터 확인용, 같은 이름은 모두)
	if (const std::wstring SelectNames = FCommandLine::FromProcess().GetValue(L"--select"); !SelectNames.empty())
	{
		const std::string Targets = "," + FStringConv::ToUtf8(SelectNames) + ",";
		Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Name) {
			if (!Name.Name.empty() && !Scene.GetRegistry().Has<FTransientComponent>(Entity) && Targets.find("," + Name.Name + ",") != std::string::npos)
			{
				Context.AddToSelection(Entity);
			}
		});
	}

	// --content-dir <Content 기준 폴더>: 콘텐츠 브라우저 시작 폴더 (자동 검증)
	if (const std::wstring ContentDir = FCommandLine::FromProcess().GetValue(L"--content-dir"); !ContentDir.empty())
	{
		ContentBrowserPanel.SetCurrentDirectory(Context.ContentDirectory / ContentDir);
	}
	// --reset-layout: 저장된 창 배치를 무시하고 기본 레이아웃으로 시작
	bResetLayoutRequested = FCommandLine::FromProcess().HasFlag(L"--reset-layout");

	// 자동 검증: --open-asset <Content 기준 경로>[,<경로>...] 으로 시작 시 에셋 편집 창 열기
	if (const std::wstring AssetArgs = FCommandLine::FromProcess().GetValue(L"--open-asset"); !AssetArgs.empty())
	{
		size_t Start = 0;
		while (Start <= AssetArgs.size())
		{
			const size_t       End  = AssetArgs.find(L',', Start);
			const std::wstring Item = AssetArgs.substr(Start, End == std::wstring::npos ? std::wstring::npos : End - Start);
			if (!Item.empty())
			{
				Context.OpenAssetEditorRequest(Context.ContentDirectory / Item);
			}
			if (End == std::wstring::npos)
			{
				break;
			}
			Start = End + 1;
		}
	}

	// 자동 검증: --verify-undo 복제 → 커밋 → 실행 취소 → 다시 실행 → 실행 취소를 수행하고 엔티티/메시 수를 확인
	if (FCommandLine::FromProcess().HasFlag(L"--verify-undo"))
	{
		const uint32 EntitiesBefore = Scene.GetRegistry().GetAliveCount();
		const size_t MeshesBefore   = Resources.GetMeshCount();
		FEditorActions::DuplicateSelection(Context);
		CommitPendingEdit();
		const uint32 EntitiesDuplicated = Scene.GetRegistry().GetAliveCount();
		UndoEdit();
		const bool bUndoOk = Scene.GetRegistry().GetAliveCount() == EntitiesBefore;
		RedoEdit();
		const bool bRedoOk = Scene.GetRegistry().GetAliveCount() == EntitiesDuplicated;
		UndoEdit();
		RedoEdit(); // 화면 확인용: 복제된 상태로 끝낸다
		const bool bNoNewMeshes = Resources.GetMeshCount() == MeshesBefore;
		const std::string Summary = std::format("엔티티 {} → 복제 {} → 취소 {} / 다시 {}, 메시 {} → {}, 선택 {}개", EntitiesBefore, EntitiesDuplicated,
		                                        bUndoOk ? "일치" : "불일치", bRedoOk ? "일치" : "불일치", MeshesBefore,
		                                        Resources.GetMeshCount(), Context.Selection.Num());
		if (bUndoOk && bRedoOk && bNoNewMeshes && EntitiesDuplicated > EntitiesBefore)
		{
			E_LOG(LogEditor, Display, "Undo 검증 통과: {}", Summary);
		}
		else
		{
			E_LOG(LogEditor, Error, "Undo 검증 실패: {}", Summary);
		}
	}

	// 셰이더 핫 리로드: 엔진 셰이더 디렉터리 감시 (실패해도 에디터는 계속)
	if (!ShaderWatcher.Start(FPaths::GetEngineShaderDirectory(), true))
	{
		E_LOG(LogEditor, Warning, "셰이더 디렉터리 감시를 시작하지 못했습니다. Ctrl+R로 수동 다시 로드만 가능합니다");
	}

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 10.0f, 100000.0f); // cm: 근평면 10cm, 원평면 1km
	Camera.SetPosition(FVector3(-600.0f, -400.0f, 300.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 80.0f));
	LoadEditorCamera();

	// 스크립트 핫 리로드: 프로젝트 Content의 .lua 감시
	if (!ScriptWatcher.Start(Context.ContentDirectory, true))
	{
		E_LOG(LogEditor, Warning, "Content 디렉터리 감시를 시작하지 못했습니다. 스크립트 핫 리로드가 꺼집니다");
	}

	E_LOG(LogEditor, Display, "에디터 초기화 완료. 뷰포트: 우클릭 + WASD/QE 시점, 좌클릭 선택, W/E/R 기즈모, Ctrl+N/O/S 씬 파일, F5 재생/정지");

	// 자동 검증: --play 로 시작 시 플레이 모드 진입
	if (CommandLine.HasFlag(L"--play"))
	{
		StartPlay();
	}
	return true;
}

void FEditorApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();

	if (InputState.IsKeyPressed(EKey::Escape) && !ImGuiLayer.WantCaptureKeyboard())
	{
		// 플레이 중 ESC는 플레이 정지 (UE와 동일)
		if (PlayMode.IsActive())
		{
			StopPlay();
		}
		else
		{
			RequestExit();
		}
	}

	// 뷰포트 위에서만 카메라 조작 (기즈모 사용 중 제외). 게임 카메라로 보는 중에는 조작하지 않는다
	if (ViewportPanel.IsHovered() && !ViewportPanel.IsUsingGizmo() && Context.Camera == &Camera)
	{
		CameraController.Update(Camera, InputState, DeltaSeconds);
	}

	// Delete: 선택 엔티티 삭제 (텍스트 입력 중, 에셋 편집 창 포커스 중 제외)
	FEditorActions::PruneSelection(Context);
	if (InputState.IsKeyPressed(EKey::Delete) && !ImGuiLayer.WantCaptureKeyboard() && !AssetEditors.HasFocusedEditor() && !ContentBrowserPanel.IsFocused())
	{
		FEditorActions::DeleteSelection(Context);
	}

	UpdatePlayMode(DeltaSeconds);
	FAnimationSystem::Update(*Context.Scene, DeltaSeconds);
	Context.Scene->UpdateTransforms();
	// 파티클은 편집 중에도 재생해 보여준다 (플레이 중이면 플레이 씬)
	FSceneAssetResolver::ResolveParticles(*Context.Scene, Resources, Context.ContentDirectory);
	FParticleSystem::Update(*Context.Scene, DeltaSeconds);
	AssetEditors.Update(Context, DeltaSeconds);

	PollShaderChanges();
	PollScriptChanges();

	const float InstantFps = DeltaSeconds > 0.0f ? 1.0f / DeltaSeconds : 0.0f;
	SmoothedFps            = SmoothedFps <= 0.0f ? InstantFps : FMath::Lerp(SmoothedFps, InstantFps, 0.05f);
}

void FEditorApplication::OnRender()
{
	// 뷰포트 크기 변경은 UI 기술 전에 반영
	ViewportPanel.PrepareFrame(Context);

	ImGuiLayer.BeginFrame();
	ApplyDefaultLayoutIfNeeded();
	// 윈도우 탐색기에서 끌어 놓은 파일은 콘텐츠 브라우저의 현재 폴더로 가져온다
	if (const std::vector<std::filesystem::path> Dropped = ImGuiLayer.ConsumeDroppedFiles(); !Dropped.empty())
	{
		ContentBrowserPanel.ImportExternalFiles(Context, Dropped);
	}
	HandleShortcuts();
	HandleToolShortcuts();
	HandlePlayShortcuts();
	DrawMainMenuBar();
	ViewportPanel.Draw(Context, GetInput());
	HierarchyPanel.Draw(Context);
	InspectorPanel.Draw(Context);
	// 아래 탭 묶음(통계/포스트/그림자/출력 로그/콘텐츠)은 처음에 마지막으로 그린 창이 선택되므로 콘텐츠를 마지막에
	if (bShowStats)
	{
		DrawStatsWindow();
	}
	PostProcessPanel.Draw(Context);
	ShadowPanel.Draw(Context);
	OutputLogPanel.Draw(Context);
	ContentBrowserPanel.Draw(Context);
	AssetEditors.Draw(Context);
	if (const std::wstring ReimportArg = FCommandLine::FromProcess().GetValue(L"--verify-reimport"); GetFrameIndex() == 20 && !ReimportArg.empty())
	{
		VerifyReimport(Context.ContentDirectory / ReimportArg);
	}
	if (GetFrameIndex() == 20 && FCommandLine::FromProcess().HasFlag(L"--verify-asset-move"))
	{
		VerifyAssetMove();
	}
	// 자동 검증: --verify-asset-close 편집 창을 몇 프레임 그린 뒤 값을 바꾸고 저장하지 않고 닫는다
	if (GetFrameIndex() == 30 && FCommandLine::FromProcess().HasFlag(L"--verify-asset-close"))
	{
		E_LOG(LogEditor, Display, "자동 검증: 편집 창 {}개 닫음", AssetEditors.VerifyCloseWithoutSave(Context));
	}
	if (bShowImGuiDemo)
	{
		ImGui::ShowDemoWindow(&bShowImGuiDemo);
	}
	DrawNotification();
	CommitPendingEdit();

	// 기즈모/인스펙터 편집이 월드 행렬에 즉시 반영되도록 갱신
	Context.Scene->UpdateTransforms();

	const float ClearColor[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
	Rhi->BeginFrame(ClearColor);
	ViewportPanel.RenderScene(Context);
	AssetEditors.RenderPreviews(Context);
	ContentBrowserPanel.RenderThumbnails(Context);

	// UI는 감마 인코딩된 색이므로 UNORM 뷰에 그린다
	Rhi->SetRenderTargetToBackBuffer(true);
	ImGuiLayer.EndFrame(Rhi->GetCommandList());

	Rhi->EndFrame();
}

void FEditorApplication::OnResize(uint32 Width, uint32 Height)
{
	if (Rhi)
	{
		Rhi->Resize(Width, Height);
	}
}

void FEditorApplication::OnShutdown()
{
	StopPlay();
	ScriptWatcher.Stop();
	Audio.Shutdown();
	SaveEditorCamera();
	ShaderWatcher.Stop();
	if (Rhi)
	{
		ImGuiLayer.Shutdown(); // 내부에서 GPU Flush
		AssetEditors.Shutdown(Context);
		ContentBrowserPanel.Shutdown(Context);
		ViewportPanel.Shutdown();
		SceneRenderer.Shutdown();
		Resources.Shutdown();
		Rhi->Shutdown();
		Rhi.reset();
	}
	GameModule.Unload(); // 등록 타입 제거 (씬의 게임 컴포넌트는 앱 소멸 시 정리, DLL은 프로세스 종료까지 유지)
}

// ---------------------------------------------------------------- 씬 파일

void FEditorApplication::NewScene()
{
	StopPlay();
	Scene.Clear();
	Context.ClearSelection();
	CurrentScenePath.clear();

	// 빈 씬에도 기본 조명은 둔다
	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun).Intensity = 3.0f;
	Scene.UpdateTransforms();

	ResetUndoHistory();
}

bool FEditorApplication::OpenScene(const std::filesystem::path& Path)
{
	StopPlay();
	Context.ClearSelection();
	if (!FSceneSerializer::LoadFromFile(Scene, Path))
	{
		return false;
	}
	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
	CurrentScenePath = Path;
	ResetUndoHistory();
	return true;
}

bool FEditorApplication::SaveScene()
{
	if (CurrentScenePath.empty())
	{
		return SaveSceneAs();
	}
	if (!FSceneSerializer::SaveToFile(Scene, CurrentScenePath))
	{
		return false;
	}
	UndoHistory.MarkSaved();
	UpdateWindowTitle();
	return true;
}

bool FEditorApplication::SaveSceneAs()
{
	const std::filesystem::path Path = ShowSceneFileDialog(GetWindow().GetHandle(), Context.ContentDirectory, true);
	if (Path.empty())
	{
		return false;
	}
	if (!FSceneSerializer::SaveToFile(Scene, Path))
	{
		return false;
	}
	CurrentScenePath = Path;
	UndoHistory.MarkSaved();
	UpdateWindowTitle();
	return true;
}

void FEditorApplication::OpenStartupScene()
{
	// --scene <Content 기준 상대 경로>: 기본 씬 대신 지정 씬 (자동 검증/데모용)
	if (const std::wstring SceneArg = FCommandLine::FromProcess().GetValue(L"--scene"); !SceneArg.empty())
	{
		if (OpenScene(Context.ContentDirectory / SceneArg))
		{
			return;
		}
		E_LOG(LogEditor, Warning, "--scene 씬을 열지 못해 기본 씬을 엽니다: {}", FStringConv::ToUtf8(SceneArg));
	}

	if (FPaths::HasProject() && !FPaths::GetProjectDescriptor().DefaultScene.empty())
	{
		const std::filesystem::path ScenePath = Context.ContentDirectory / FStringConv::ToWide(FPaths::GetProjectDescriptor().DefaultScene);
		if (std::filesystem::exists(ScenePath))
		{
			if (OpenScene(ScenePath))
			{
				return;
			}
			E_LOG(LogEditor, Warning, "기본 씬을 열지 못해 기본 구성 씬을 만듭니다");
		}
		else
		{
			// 프로젝트가 가리키는 기본 씬이 아직 없으면 기본 구성으로 생성해 준다
			BuildDefaultScene();
			if (FSceneSerializer::SaveToFile(Scene, ScenePath))
			{
				CurrentScenePath = ScenePath;
				E_LOG(LogEditor, Display, "기본 씬 생성: {}", FPaths::GetProjectDescriptor().DefaultScene);
			}
			ResetUndoHistory();
			return;
		}
	}

	BuildDefaultScene();
	ResetUndoHistory();
}

void FEditorApplication::UpdateWindowTitle()
{
	std::wstring Title = L"ProjectE Editor";
	Title += FPaths::HasProject() ? L" - " + FStringConv::ToWide(FPaths::GetProjectName()) : L" (프로젝트 없음)";
	Title += L" - " + (CurrentScenePath.empty() ? std::wstring(L"제목 없음") : CurrentScenePath.filename().wstring());
	if (UndoHistory.IsDirty())
	{
		Title += L" *"; // 저장하지 않은 변경
	}
	GetWindow().SetTitle(Title);
}

void FEditorApplication::BuildDefaultScene()
{
	Scene.Clear();
	Context.ClearSelection();
	CurrentScenePath.clear();

	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
	SunLight.Intensity = 3.0f;

	// 바닥: 내장 큐브 + 체커 머티리얼 에셋 (에셋 참조로 기록되어 저장/로드 가능)
	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
	FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
	GroundMesh.MeshAsset     = "primitive:cube";
	GroundMesh.MaterialAsset = "Materials/Checker.emat";

	// 모델: 루트 엔티티 + 에셋 경로 (자식은 Resolve에서 생성)
	const FEntity Helmet = Scene.CreateEntity("DamagedHelmet");
	Scene.GetTransform(Helmet).Position = FVector3(0.0f, 0.0f, 120.0f);
	Scene.GetRegistry().Emplace<FModelComponent>(Helmet).AssetPath = "DamagedHelmet.glb";

	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
}

// ---------------------------------------------------------------- UI

void FEditorApplication::HandleShortcuts()
{
	if (ImGui::GetIO().WantTextInput)
	{
		return;
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
	{
		NewScene();
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
	{
		const std::filesystem::path Path = ShowSceneFileDialog(GetWindow().GetHandle(), Context.ContentDirectory, false);
		if (!Path.empty())
		{
			OpenScene(Path);
		}
	}
	// 에셋 편집 창이 포커스를 가지면 저장/실행 취소는 그 창이 처리한다
	if (AssetEditors.HasFocusedEditor())
	{
		return;
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
	{
		SaveSceneAs();
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
	{
		SaveScene();
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
	{
		RedoEdit();
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
	{
		UndoEdit();
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
	{
		FEditorActions::DuplicateSelection(Context);
	}
}

void FEditorApplication::DrawMainMenuBar()
{
	if (!ImGui::BeginMainMenuBar())
	{
		return;
	}

	if (ImGui::BeginMenu("파일"))
	{
		if (ImGui::MenuItem("새 씬", "Ctrl+N"))
		{
			NewScene();
		}
		if (ImGui::MenuItem("씬 열기...", "Ctrl+O"))
		{
			const std::filesystem::path Path = ShowSceneFileDialog(GetWindow().GetHandle(), Context.ContentDirectory, false);
			if (!Path.empty())
			{
				OpenScene(Path);
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("저장", "Ctrl+S"))
		{
			SaveScene();
		}
		if (ImGui::MenuItem("다른 이름으로 저장...", "Ctrl+Shift+S"))
		{
			SaveSceneAs();
		}
		ImGui::Separator();
		if (!CurrentScenePath.empty())
		{
			ImGui::TextDisabled("%s", FStringConv::ToUtf8(CurrentScenePath.filename().wstring()).c_str());
			ImGui::Separator();
		}
		if (ImGui::MenuItem("종료", "ESC"))
		{
			RequestExit();
		}
		ImGui::EndMenu();
	}
	DrawEditMenu();
	if (ImGui::BeginMenu("엔티티"))
	{
		if (ImGui::MenuItem("빈 엔티티 추가"))
		{
			Context.Select(Context.Scene->CreateEntity("Entity"));
			Context.MarkEdited("엔티티 추가");
		}
		if (ImGui::MenuItem("큐브 추가"))
		{
			const FEntity         Cube = Context.Scene->CreateEntity("Cube");
			FStaticMeshComponent& Mesh = Context.Scene->GetRegistry().Emplace<FStaticMeshComponent>(Cube);
			Mesh.Mesh      = Context.DefaultCubeMesh;
			Mesh.MeshAsset = "primitive:cube";
			Context.Select(Cube);
			Context.MarkEdited("큐브 추가");
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("창"))
	{
		ImGui::MenuItem("뷰포트", nullptr, &ViewportPanel.bOpen);
		ImGui::MenuItem("계층", nullptr, &HierarchyPanel.bOpen);
		ImGui::MenuItem("인스펙터", nullptr, &InspectorPanel.bOpen);
		ImGui::MenuItem("콘텐츠", nullptr, &ContentBrowserPanel.bOpen);
		ImGui::MenuItem("포스트 프로세스", nullptr, &PostProcessPanel.bOpen);
		ImGui::MenuItem("그림자", nullptr, &ShadowPanel.bOpen);
		ImGui::MenuItem("출력 로그", nullptr, &OutputLogPanel.bOpen);
		ImGui::MenuItem("통계", nullptr, &bShowStats);
		ImGui::Separator();
		if (ImGui::MenuItem("기본 레이아웃으로 되돌리기"))
		{
			bResetLayoutRequested = true;
		}
		ImGui::MenuItem("ImGui 데모", nullptr, &bShowImGuiDemo);
		ImGui::EndMenu();
	}
	DrawToolsMenu();
	DrawPlayControls();

	ImGui::EndMainMenuBar();
}

void FEditorApplication::DrawStatsWindow()
{
	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_CHART_SIMPLE, "통계", "Stats").c_str(), &bShowStats))
	{
		const FSceneRenderStats& Stats = SceneRenderer.GetStats();
		ImGui::Text("FPS: %.1f (%.2f ms)", SmoothedFps, SmoothedFps > 0.0f ? 1000.0f / SmoothedFps : 0.0f);
		ImGui::Text("메시: %u / %u 표시, 드로우 %u", Stats.VisibleMeshes, Stats.TotalMeshes, Stats.DrawCalls);
		ImGui::Text("파티클: %u", Stats.Particles);
		ImGui::Text("엔티티: %u", Context.Scene->GetRegistry().GetAliveCount());
		ImGui::Text("리소스: 메시 %zu, 머티리얼 %zu, 텍스처 %zu", Resources.GetMeshCount(), Resources.GetMaterialCount(),
		            Resources.GetTextureCount());

		bool bVSync = Rhi->IsVSync();
		if (ImGui::Checkbox("VSync", &bVSync))
		{
			Rhi->SetVSync(bVSync);
		}
		bool bFreeze = SceneRenderer.IsCullingFrozen();
		if (ImGui::Checkbox("컬링 프러스텀 고정", &bFreeze))
		{
			SceneRenderer.SetFreezeCulling(bFreeze);
		}
	}
	ImGui::End();
}

// ---------------------------------------------------------------- 셰이더 핫 리로드

namespace
{
	bool IsShaderSourceFile(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension == L".hlsl" || Extension == L".hlsli";
	}

	// 쿠킹 산출물(Engine/Shaders/Cooked/) 변경은 무시
	bool IsInCookedDirectory(const std::filesystem::path& Path, const std::filesystem::path& ShaderDirectory)
	{
		std::error_code             ErrorCode;
		const std::filesystem::path Relative = std::filesystem::relative(Path, ShaderDirectory, ErrorCode);
		if (ErrorCode || Relative.empty())
		{
			return false;
		}
		std::wstring FirstPart = Relative.begin()->wstring();
		std::transform(FirstPart.begin(), FirstPart.end(), FirstPart.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return FirstPart == L"cooked";
	}
} // namespace

void FEditorApplication::PollShaderChanges()
{
	const std::vector<std::filesystem::path> Changed = ShaderWatcher.Poll();
	if (Changed.empty())
	{
		return;
	}

	const std::filesystem::path ShaderDirectory = FPaths::GetEngineShaderDirectory();
	std::string                 ChangedNames;
	std::vector<std::filesystem::path> ShaderFiles;
	size_t                      AffectedCount = 0;
	for (const std::filesystem::path& Path : Changed)
	{
		if (!IsShaderSourceFile(Path) || IsInCookedDirectory(Path, ShaderDirectory))
		{
			continue;
		}
		const size_t Count = SceneRenderer.GetShaderLibrary().Invalidate(Path).size();
		if (Count == 0)
		{
			E_LOG(LogEditor, Log, "셰이더 변경 감지 (사용 중인 셰이더 아님): {}", FStringConv::ToUtf8(Path.filename().wstring()));
			continue;
		}
		AffectedCount += Count;
		ShaderFiles.push_back(Path);
		ChangedNames += (ChangedNames.empty() ? "" : ", ") + FStringConv::ToUtf8(Path.filename().wstring());
	}
	if (AffectedCount == 0)
	{
		return;
	}

	const bool bMainOk    = SceneRenderer.ReloadShaders() && ViewportPanel.ReloadShaders(false);
	const bool bPreviewOk = AssetEditors.ReloadShaders(&ShaderFiles);
	ContentBrowserPanel.ReloadShaders(&ShaderFiles);
	if (bMainOk && bPreviewOk)
	{
		E_LOG(LogEditor, Display, "셰이더 다시 로드됨: {}", ChangedNames);
		ShowNotification("셰이더 다시 로드됨: " + ChangedNames, false);
	}
	else
	{
		E_LOG(LogEditor, Error, "셰이더 컴파일 실패: {} (기존 셰이더 유지)", ChangedNames);
		ShowNotification("셰이더 컴파일 실패: " + ChangedNames + " (기존 셰이더 유지, 로그 확인)", true);
	}
}

void FEditorApplication::ReloadAllShaders()
{
	SceneRenderer.GetShaderLibrary().InvalidateAll();
	const bool bMainOk    = SceneRenderer.ReloadShaders(true) && ViewportPanel.ReloadShaders(true);
	const bool bPreviewOk = AssetEditors.ReloadShaders(nullptr);
	ContentBrowserPanel.ReloadShaders(nullptr);
	if (bMainOk && bPreviewOk)
	{
		ShowNotification("셰이더 전체 다시 로드됨", false);
	}
	else
	{
		ShowNotification("셰이더 컴파일 실패 (기존 셰이더 유지, 로그 확인)", true);
	}
}

void FEditorApplication::HandleToolShortcuts()
{
	if (ImGui::GetIO().WantTextInput)
	{
		return;
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R))
	{
		ReloadAllShaders();
	}
}

void FEditorApplication::DrawToolsMenu()
{
	if (ImGui::BeginMenu("도구"))
	{
		if (ImGui::MenuItem("셰이더 다시 로드", "Ctrl+R"))
		{
			ReloadAllShaders();
		}
		ImGui::TextDisabled(ShaderWatcher.IsWatching() ? "셰이더 자동 감시: 켜짐" : "셰이더 자동 감시: 꺼짐");
		ImGui::EndMenu();
	}
}

void FEditorApplication::ShowNotification(std::string Message, bool bError)
{
	NotificationText   = std::move(Message);
	bNotificationError = bError;
	NotificationExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(bError ? 5 : 3);
}

void FEditorApplication::DrawNotification()
{
	if (NotificationText.empty() || std::chrono::steady_clock::now() >= NotificationExpiry)
	{
		return;
	}

	// 메인 창 우하단 토스트
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float          Margin   = 16.0f * ImGuiLayer.GetDpiScale();
	ImGui::SetNextWindowPos(ImVec2(Viewport->WorkPos.x + Viewport->WorkSize.x - Margin, Viewport->WorkPos.y + Viewport->WorkSize.y - Margin),
	                        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowViewport(Viewport->ID);
	ImGui::SetNextWindowBgAlpha(0.85f);
	constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
	                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
	                                   ImGuiWindowFlags_NoSavedSettings;
	if (ImGui::Begin("##Notification", nullptr, Flags))
	{
		const ImVec4 Color = bNotificationError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.55f, 0.9f, 0.55f, 1.0f);
		ImGui::TextColored(Color, "%s", NotificationText.c_str());
	}
	ImGui::End();
}

void FEditorApplication::OnScreenshotRequested(const std::filesystem::path& Path)
{
	if (Rhi)
	{
		Rhi->RequestScreenshot(Path);
	}
}

// ---------------------------------------------------------------- 실행 취소

void FEditorApplication::ResetUndoHistory()
{
	Context.PendingEdit = FPendingEdit{};
	UndoHistory.Reset(FSceneSerializer::ToJsonString(Scene));
	UpdateWindowTitle();
}

void FEditorApplication::CommitPendingEdit()
{
	// 플레이 중 변경은 플레이 씬에 대한 것이므로 편집 기록에 남기지 않는다 (정지하면 버려짐)
	if (PlayMode.IsActive())
	{
		Context.PendingEdit = FPendingEdit{};
		return;
	}
	// 드래그/텍스트 입력/기즈모 조작이 이어지는 동안은 모아 두었다가 끝난 뒤 한 단계로 기록
	const bool  bInteracting = ImGui::IsAnyItemActive() || ViewportPanel.IsUsingGizmo();
	std::string Label;
	if (!Context.PendingEdit.TryTake(bInteracting, Label))
	{
		return;
	}
	if (UndoHistory.Commit(std::move(Label), FSceneSerializer::ToJsonString(Scene)))
	{
		UpdateWindowTitle();
	}
}

void FEditorApplication::UndoEdit()
{
	if (PlayMode.IsActive())
	{
		ShowNotification("플레이 중에는 실행 취소할 수 없습니다", true);
		return;
	}
	CommitPendingEdit();
	if (Context.PendingEdit.IsPending() || !UndoHistory.CanUndo())
	{
		return; // 조작 중이거나 되돌릴 것이 없음
	}
	const std::string Label = UndoHistory.GetUndoLabel();
	RestoreSnapshot(*UndoHistory.Undo());
	ShowNotification("실행 취소: " + Label, false);
}

void FEditorApplication::RedoEdit()
{
	if (PlayMode.IsActive())
	{
		ShowNotification("플레이 중에는 다시 실행할 수 없습니다", true);
		return;
	}
	CommitPendingEdit();
	if (Context.PendingEdit.IsPending() || !UndoHistory.CanRedo())
	{
		return;
	}
	const std::string Label = UndoHistory.GetRedoLabel();
	RestoreSnapshot(*UndoHistory.Redo());
	ShowNotification("다시 실행: " + Label, false);
}

void FEditorApplication::RestoreSnapshot(const std::string& State)
{
	// 엔티티 핸들은 복원 후 바뀌므로 선택을 경로로 기억해 둔다
	std::vector<FEntityPath> SelectedPaths;
	for (FEntity Entity : Context.Selection.GetEntities())
	{
		SelectedPaths.push_back(FEntityPath::Build(Scene, Entity));
	}
	const FEntityPath PrimaryPath = FEntityPath::Build(Scene, Context.SelectedEntity);

	ModelTemplates.Capture(Scene);
	if (!FSceneSerializer::FromJsonString(Scene, State))
	{
		E_LOG(LogEditor, Error, "실행 취소 스냅샷 복원 실패");
	}
	ModelTemplates.Instantiate(Scene);
	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
	Scene.UpdateTransforms();

	std::vector<FEntity> Restored;
	for (const FEntityPath& Path : SelectedPaths)
	{
		if (const FEntity Entity = Path.Resolve(Scene); Entity.IsValid())
		{
			Restored.push_back(Entity);
		}
	}
	Context.SelectMany(Restored, PrimaryPath.Resolve(Scene));
	UpdateWindowTitle();
}

void FEditorApplication::DrawEditMenu()
{
	if (!ImGui::BeginMenu("편집"))
	{
		return;
	}
	const std::string UndoText = UndoHistory.CanUndo() ? "실행 취소: " + UndoHistory.GetUndoLabel() : std::string("실행 취소");
	const std::string RedoText = UndoHistory.CanRedo() ? "다시 실행: " + UndoHistory.GetRedoLabel() : std::string("다시 실행");
	if (ImGui::MenuItem(UndoText.c_str(), "Ctrl+Z", false, UndoHistory.CanUndo()))
	{
		UndoEdit();
	}
	if (ImGui::MenuItem(RedoText.c_str(), "Ctrl+Y", false, UndoHistory.CanRedo()))
	{
		RedoEdit();
	}
	ImGui::Separator();
	const bool bHasSelection = !Context.Selection.IsEmpty();
	if (ImGui::MenuItem("복제", "Ctrl+D", false, bHasSelection))
	{
		FEditorActions::DuplicateSelection(Context);
	}
	if (ImGui::MenuItem("삭제", "Del", false, bHasSelection))
	{
		FEditorActions::DeleteSelection(Context);
	}
	if (ImGui::MenuItem("선택 항목 포커스", "F", false, bHasSelection))
	{
		ViewportPanel.FocusSelection(Context);
	}
	ImGui::Separator();
	ImGui::MenuItem("그리드 표시", nullptr, &ViewportPanel.bShowGrid);
	ImGui::MenuItem("기즈모 스냅", nullptr, &ViewportPanel.Snap.bEnabled);
	ImGui::TextDisabled("실행 취소 %zu단계 / 다시 실행 %zu단계", UndoHistory.GetUndoCount(), UndoHistory.GetRedoCount());
	ImGui::EndMenu();
}

// ---------------------------------------------------------------- 에디터 카메라

namespace
{
	std::filesystem::path GetEditorCameraPath()
	{
		return FPaths::GetSavedDirectory() / L"EditorCamera.json";
	}
} // namespace

void FEditorApplication::LoadEditorCamera()
{
	FEditorCameraState State;
	if (!State.Load(GetEditorCameraPath()))
	{
		return;
	}
	Camera.SetPosition(State.Position);
	Camera.SetRotation(State.Rotation);
	CameraController.MoveSpeed = State.MoveSpeed;
	CameraController.SyncFromCamera(Camera);
	E_LOG(LogEditor, Log, "에디터 카메라 복원: ({:.0f}, {:.0f}, {:.0f})", State.Position.X, State.Position.Y, State.Position.Z);
}

void FEditorApplication::SaveEditorCamera() const
{
	FEditorCameraState State;
	State.Position  = Camera.GetPosition();
	State.Rotation  = Camera.GetRotation();
	State.MoveSpeed = CameraController.MoveSpeed;
	State.Save(GetEditorCameraPath());
}

// ---------------------------------------------------------------- 플레이 모드 / 스크립트

void FEditorApplication::StartPlay()
{
	if (PlayMode.IsActive())
	{
		return;
	}
	PlayMode.Play(Context);
	ShowNotification("플레이 시작 — F5/ESC 정지, F6 일시정지, F7 한 프레임", false);
}

void FEditorApplication::StopPlay()
{
	if (!PlayMode.IsActive())
	{
		return;
	}
	PlayMode.Stop(Context);
	AudioSystem.Reset(Audio);
	// 플레이 중 뷰포트 크기가 바뀌었을 수 있으므로 에디터 카메라 종횡비를 맞춘다
	Context.Camera = &Camera;
	Camera.SetAspectRatio(ViewportPanel.GetAspectRatio(Camera.GetAspectRatio()));
}

void FEditorApplication::UpdatePlayMode(float DeltaSeconds)
{
	// 텍스트 입력 중에는 게임에 키 입력을 주지 않는다
	const bool bGameInput = !ImGui::GetIO().WantTextInput;
	PlayMode.Tick(Context, DeltaSeconds, bGameInput ? &GetInput() : nullptr);

	// 주 카메라 컴포넌트가 있으면 그 시점으로 보고, 없으면 에디터 카메라
	FCamera* GameCamera = PlayMode.UpdateGameCamera(ViewportPanel.GetAspectRatio(Camera.GetAspectRatio()));
	Context.Camera      = GameCamera != nullptr ? GameCamera : &Camera;

	// 오디오: 플레이 중에만, 보고 있는 카메라가 청자 (트랜스폼은 직전 프레임 갱신 기준)
	if (PlayMode.IsActive())
	{
		Audio.SetListener({ Context.Camera->GetPosition(), Context.Camera->GetForwardVector(), Context.Camera->GetUpVector() });
		AudioSystem.Update(*Context.Scene, Audio, Context.ContentDirectory);
	}
}

void FEditorApplication::HandlePlayShortcuts()
{
	if (ImGui::GetIO().WantTextInput)
	{
		return;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
	{
		if (PlayMode.IsActive())
		{
			StopPlay();
		}
		else
		{
			StartPlay();
		}
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
	{
		PlayMode.TogglePause();
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F7))
	{
		PlayMode.RequestStep();
	}
}

void FEditorApplication::DrawPlayControls()
{
	// 메뉴 바 가운데 아이콘 버튼 (언리얼처럼 재생/일시정지/한 프레임/정지 자리가 고정)
	const float ButtonWidth = ImGui::GetFrameHeight() * 1.4f;
	const float GroupWidth  = ButtonWidth * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
	ImGui::SetCursorPosX(FMath::Max(ImGui::GetCursorPosX() + 16.0f, (ImGui::GetWindowWidth() - GroupWidth) * 0.5f));

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
	const auto IconButton = [&](const char* Icon, const ImVec4& Color, bool bEnabled, const char* Tooltip) {
		ImGui::BeginDisabled(!bEnabled);
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		const bool bClicked = ImGui::Button(Icon, ImVec2(ButtonWidth, 0.0f));
		ImGui::PopStyleColor();
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("%s", Tooltip);
		return bClicked;
	};

	const bool bActive = PlayMode.IsActive();
	const bool bPaused = bActive && PlayMode.IsPaused();
	if (!bActive || bPaused)
	{
		if (IconButton(ICON_FA_PLAY, FEditorTheme::Success, true, bActive ? "계속 (F6)" : "뷰포트에서 재생 (F5). 정지하면 씬이 재생 전 상태로 돌아갑니다"))
		{
			bActive ? PlayMode.TogglePause() : StartPlay();
		}
	}
	else if (IconButton(ICON_FA_PAUSE, FEditorTheme::Warning, true, "일시정지 (F6)"))
	{
		PlayMode.TogglePause();
	}
	if (IconButton(ICON_FA_FORWARD_STEP, ImVec4(0.8f, 0.8f, 0.8f, 1.0f), bPaused, "일시정지 중 한 프레임 진행 (F7)"))
	{
		PlayMode.RequestStep();
	}
	if (IconButton(ICON_FA_STOP, FEditorTheme::Danger, bActive, "정지하고 편집 씬 복원 (F5 / ESC)"))
	{
		StopPlay();
	}
	ImGui::PopStyleColor(2);

	if (bActive)
	{
		ImGui::TextColored(bPaused ? FEditorTheme::Warning : FEditorTheme::Success, bPaused ? "일시정지됨" : "플레이 중");
	}
	if (bActive && Scripts.GetErrorCount() > 0)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_TRIANGLE_EXCLAMATION " 스크립트 오류 %u건", Scripts.GetErrorCount());
	}
}

void FEditorApplication::PollScriptChanges()
{
	for (const std::filesystem::path& Path : ScriptWatcher.Poll())
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		if (Extension != L".lua")
		{
			continue;
		}
		const std::string Name = FStringConv::ToUtf8(Path.filename().wstring());
		if (Scripts.ReloadScript(Path))
		{
			ShowNotification("스크립트 다시 로드됨: " + Name, false);
		}
		else
		{
			ShowNotification("스크립트 오류: " + Name + " (기존 코드 유지, 로그 확인)", true);
		}
	}
}

void FEditorApplication::ApplyDefaultLayoutIfNeeded()
{
	// 레이아웃 저장 파일에 뷰포트 창 기록이 없으면(첫 실행, 창 ID 변경 후) 또는 메뉴로 요청하면 기본 배치
	if (!bResetLayoutRequested && (bLayoutChecked || ImGui::FindWindowSettingsByID(ImHashStr("###Viewport")) != nullptr))
	{
		bLayoutChecked = true;
		return;
	}
	bLayoutChecked        = true;
	bResetLayoutRequested = false;

	// 언리얼 5 배치: 가운데 뷰포트, 오른쪽 위 계층 / 오른쪽 아래 인스펙터, 아래 콘텐츠·출력 로그·통계·렌더 설정 탭
	const ImGuiID        DockSpace = ImGuiLayer.GetDockSpaceId();
	const ImGuiViewport* Viewport  = ImGui::GetMainViewport();
	ImGui::DockBuilderRemoveNode(DockSpace);
	ImGui::DockBuilderAddNode(DockSpace, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(DockSpace, Viewport->WorkSize);

	ImGuiID Main        = 0;
	ImGuiID Center      = 0;
	ImGuiID RightTop    = 0;
	const ImGuiID Right       = ImGui::DockBuilderSplitNode(DockSpace, ImGuiDir_Right, 0.26f, nullptr, &Main);
	const ImGuiID Bottom      = ImGui::DockBuilderSplitNode(Main, ImGuiDir_Down, 0.38f, nullptr, &Center);
	const ImGuiID RightBottom = ImGui::DockBuilderSplitNode(Right, ImGuiDir_Down, 0.60f, nullptr, &RightTop);

	ImGui::DockBuilderDockWindow("###Viewport", Center);
	ImGui::DockBuilderDockWindow("###ContentBrowser", Bottom);
	ImGui::DockBuilderDockWindow("###OutputLog", Bottom);
	ImGui::DockBuilderDockWindow("###Hierarchy", RightTop);
	ImGui::DockBuilderDockWindow("###Stats", Bottom);
	ImGui::DockBuilderDockWindow("###Inspector", RightBottom);
	ImGui::DockBuilderDockWindow("###PostProcess", Bottom);
	ImGui::DockBuilderDockWindow("###Shadows", Bottom);
	ImGui::DockBuilderFinish(DockSpace);
}

void FEditorApplication::OnAssetsMoved(const std::vector<FAssetMove>& Moves)
{
	// 1) 리소스 캐시 키 → 새 경로 (같은 에셋을 새 경로로 다시 로드해 중복 생성하지 않게)
	for (const FAssetMove& Move : Moves)
	{
		Resources.OnAssetMoved(Move.From, Move.To);
	}

	// 2) 열린 씬의 컴포넌트 문자열 (디스크의 씬 파일은 참조 갱신기가 이미 고쳤으므로 편집 기록은 남기지 않는다)
	uint32               Remapped = 0;
	FRegistry&           Registry = Scene.GetRegistry();
	std::vector<FEntity> Entities;
	if (const TSparseSet<FHierarchyComponent>* Pool = Registry.TryGetPool<FHierarchyComponent>())
	{
		Entities = Pool->GetEntities();
	}
	FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
		for (FEntity Entity : Entities)
		{
			if (!Type.HasComponent(Registry, Entity))
			{
				continue;
			}
			void* Component = Type.GetComponent(Registry, Entity);
			for (const FPropertyInfo& Property : Type.Properties)
			{
				if (Property.Type != EPropertyType::String)
				{
					continue;
				}
				std::string& Value = Property.GetRef<std::string>(Component);
				if (const std::optional<std::string> NewValue = FAssetReferenceUpdater::RemapContentPath(Value, Context.ContentDirectory, Moves))
				{
					Value = *NewValue;
					++Remapped;
				}
			}
		}
	});

	// 3) 실행 취소 기록의 씬 스냅샷도 같은 경로로 (되돌려도 옛 경로를 찾지 않게)
	UndoHistory.TransformStates([&](std::string& State) { FAssetReferenceUpdater::RemapSceneJson(State, Context.ContentDirectory, Moves); });

	// 4) 열려 있는 씬 파일 자체가 옮겨졌으면 저장 경로도
	if (!CurrentScenePath.empty())
	{
		if (const std::optional<std::filesystem::path> NewScenePath = FAssetReferenceUpdater::MapPath(CurrentScenePath, Moves))
		{
			CurrentScenePath = *NewScenePath;
			UpdateWindowTitle();
		}
	}
	E_LOG(LogEditor, Display, "에셋 이동 반영: {}건, 열린 씬 참조 {}곳", Moves.size(), Remapped);
}

void FEditorApplication::VerifyAssetMove()
{
	// 자동 검증 (--verify-asset-move): Content/_VerifyAssetMove에 샘플 복사본을 만들고, 콘텐츠 브라우저와 같은 경로로
	// 폴더 이동 + 텍스처 이름 변경을 한 뒤 씬 파일/머티리얼/열린 씬의 참조가 새 경로인지 확인한다. 끝나면 복사본을 지운다
	namespace fs                 = std::filesystem;
	const fs::path  Root         = Context.ContentDirectory / L"_VerifyAssetMove";
	std::error_code ErrorCode;
	fs::remove_all(Root, ErrorCode);
	fs::create_directories(Root / L"Moved", ErrorCode);
	fs::copy(Context.ContentDirectory / L"Materials", Root / L"Materials", fs::copy_options::recursive, ErrorCode);
	fs::copy_file(Context.ContentDirectory / L"UVChecker.png", Root / L"UVChecker.png", ErrorCode);
	{
		std::ofstream File(Root / L"Test.escene", std::ios::binary);
		File << "{ \"Version\": 1, \"Entities\": [ { \"Name\": \"Box\", \"Parent\": -1, \"Components\": { \"StaticMeshComponent\": "
		        "{ \"MeshAsset\": \"primitive:cube\", \"MaterialAsset\": \"_VerifyAssetMove/Materials/Checker.emat\", \"Visible\": true } } } ] }";
	}
	const auto ReadText = [](const fs::path& Path) {
		std::ifstream     File(Path, std::ios::binary);
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		return Buffer.str();
	};
	const auto LiveMaterialAsset = [this]() {
		std::string Found;
		Scene.GetRegistry().View<FStaticMeshComponent>().Each([&](FEntity, FStaticMeshComponent& Mesh) {
			if (Mesh.MaterialAsset.find("_VerifyAssetMove") != std::string::npos)
			{
				Found = Mesh.MaterialAsset;
			}
		});
		return Found;
	};

	bool bOk = OpenScene(Root / L"Test.escene");
	const size_t MaterialsBefore = Resources.GetMaterialCount();
	bOk = bOk && ContentBrowserPanel.MoveAssets(Context, { Root / L"Materials" }, Root / L"Moved");
	const bool bSceneFile = ReadText(Root / L"Test.escene").find("\"_VerifyAssetMove/Moved/Materials/Checker.emat\"") != std::string::npos;
	const bool bLive      = LiveMaterialAsset() == "_VerifyAssetMove/Moved/Materials/Checker.emat";
	const bool bOwnRef    = ReadText(Root / L"Moved/Materials/Checker.emat").find("\"../../UVChecker.png\"") != std::string::npos;
	bOk = bOk && ContentBrowserPanel.RenameAsset(Context, Root / L"UVChecker.png", L"Renamed.png");
	const bool bRenamed = ReadText(Root / L"Moved/Materials/Checker.emat").find("\"../../Renamed.png\"") != std::string::npos;
	// 새 경로로 다시 해석해도 캐시 키가 옮겨졌으므로 머티리얼이 새로 생기지 않는다
	Resources.LoadMaterial(Root / L"Moved/Materials/Checker.emat");
	const bool bNoDuplicate = Resources.GetMaterialCount() == MaterialsBefore;

	const bool bPassed = bOk && bSceneFile && bLive && bOwnRef && bRenamed && bNoDuplicate;
	const std::string Summary = std::format("씬 파일 {}, 열린 씬 {}, 머티리얼 자기 참조 {}, 이름 변경 {}, 중복 없음 {}", bSceneFile, bLive, bOwnRef, bRenamed, bNoDuplicate);
	if (bPassed)
	{
		E_LOG(LogEditor, Display, "자동 검증 (에셋 이동): 통과 — {}", Summary);
	}
	else
	{
		E_LOG(LogEditor, Error, "자동 검증 (에셋 이동): 실패 — {}", Summary);
	}

	NewScene();
	fs::remove_all(Root, ErrorCode);
	fs::remove_all(FPaths::GetProjectDirectory() / L"Cooked" / L"_VerifyAssetMove", ErrorCode); // 복사본이 만든 쿠킹 캐시
}

bool FEditorApplication::ReimportModelAsset(const std::filesystem::path& Path)
{
	if (PlayMode.IsActive())
	{
		ShowNotification("플레이 중에는 다시 가져올 수 없습니다", true);
		return false;
	}
	// 1) 캐시에서 옛 리소스를 꺼내 둔다 (인스턴스를 새로 만든 뒤 해제)
	std::unique_ptr<FModelResources> Old = Resources.TakeModelResources(Path);

	// 2) 열린 씬에서 같은 모델 인스턴스의 생성 노드를 지우고 다시 해석 (루트와 그 컴포넌트는 유지)
	std::error_code             ErrorCode;
	const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	std::vector<FEntity>        Roots;
	Scene.GetRegistry().View<FModelComponent>().Each([&](FEntity Entity, FModelComponent& Model) {
		const std::filesystem::path AssetPath = FStringConv::ToWide(Model.AssetPath);
		std::error_code             LocalError;
		if (std::filesystem::weakly_canonical(AssetPath.is_absolute() ? AssetPath : Context.ContentDirectory / AssetPath, LocalError) == Canonical)
		{
			Roots.push_back(Entity);
		}
	});
	for (const FEntity Root : Roots)
	{
		const std::vector<FEntity> Children = Scene.GetChildren(Root);
		for (const FEntity Child : Children)
		{
			Scene.DestroyEntity(Child);
		}
	}
	ModelTemplates.Clear(); // 실행 취소용 모델 템플릿도 옛 리소스를 가리키므로 버린다
	FEditorActions::PruneSelection(Context);
	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);

	// 3) 편집 창 미리보기 / 썸네일
	AssetEditors.OnModelReimported(Context, Path);
	ContentBrowserPanel.InvalidateThumbnail(Path);

	// 4) 옛 GPU 리소스 해제 (진행 중 프레임이 쓸 수 있으므로 지연)
	if (Old)
	{
		Resources.DestroyModelResources(*Old);
	}
	const bool bLoaded = FModelLoader::LoadModelResources(Path, Resources) != nullptr;
	ShowNotification(bLoaded ? std::format("다시 가져옴: {} (씬 인스턴스 {}개 갱신)", FStringConv::ToUtf8(Path.filename().wstring()), Roots.size())
	                         : "다시 가져오지 못했습니다: " + FStringConv::ToUtf8(Path.filename().wstring()),
	                 !bLoaded);
	return bLoaded;
}

void FEditorApplication::VerifyReimport(const std::filesystem::path& ModelPath)
{
	// 자동 검증 (--verify-reimport <모델>): 모델을 씬에 놓고 임포트 크기를 2배로 바꿔 다시 가져온 뒤 크기가 2배인지,
	// 원래 설정으로 되돌린 뒤 원래 크기인지 확인한다. 기존 .eimport는 백업 후 복원
	const std::filesystem::path Sidecar = FModelImportSettings::GetSidecarPath(ModelPath);
	std::error_code             ErrorCode;
	const bool                  bHadSidecar = std::filesystem::exists(Sidecar, ErrorCode);
	std::string                 Backup;
	if (bHadSidecar)
	{
		std::ifstream     File(Sidecar, std::ios::binary);
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		Backup = Buffer.str();
	}
	const FEntity Root = FModelLoader::LoadIntoScene(ModelPath, Scene, Resources);
	const auto    MeasureWidth = [&]() {
		Scene.UpdateTransforms();
		FBox Bounds;
		Scene.GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& Mesh) {
			const FStaticMesh* StaticMesh = Resources.GetMesh(Mesh.Mesh);
			if (StaticMesh != nullptr && Scene.IsAncestorOf(Root, Entity))
			{
				Bounds.AddBox(StaticMesh->GetLocalBounds().TransformBy(Transform.WorldMatrix));
			}
		});
		return Bounds.IsValid() ? Bounds.GetSize().Length() : 0.0f;
	};

	const float          Before   = MeasureWidth();
	FModelImportSettings Settings = FModelImportSettings::LoadForSource(ModelPath);
	const float          OldScale = Settings.Scale;
	Settings.Scale                = OldScale * 2.0f;
	Settings.SaveForSource(ModelPath);
	const bool  bFirst  = ReimportModelAsset(ModelPath);
	const float Doubled = MeasureWidth();

	if (bHadSidecar)
	{
		std::ofstream(Sidecar, std::ios::binary | std::ios::trunc) << Backup;
	}
	else
	{
		std::filesystem::remove(Sidecar, ErrorCode);
	}
	const bool  bSecond  = ReimportModelAsset(ModelPath);
	const float Restored = MeasureWidth();
	Scene.DestroyEntity(Root);

	const float Ratio   = Before > 0.0f ? Doubled / Before : 0.0f;
	const bool  bPassed = bFirst && bSecond && FMath::IsNearlyEqual(Ratio, 2.0f, 0.05f) && FMath::IsNearlyEqual(Restored, Before, Before * 0.01f + 0.01f);
	const std::string Summary = std::format("{}: 크기 {:.1f} → {:.1f} (x{:.2f}) → 복원 {:.1f}", FStringConv::ToUtf8(ModelPath.filename().wstring()), Before, Doubled, Ratio, Restored);
	if (bPassed)
	{
		E_LOG(LogEditor, Display, "자동 검증 (다시 가져오기): 통과 — {}", Summary);
	}
	else
	{
		E_LOG(LogEditor, Error, "자동 검증 (다시 가져오기): 실패 — {}", Summary);
	}
}

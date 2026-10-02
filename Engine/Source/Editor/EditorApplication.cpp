#include "Editor/EditorApplication.h"

#include "AI/AIModule.h"
#include "AI/AISystem.h"
#include "Audio/AudioReflection.h"
#include "Physics/PhysicsReflection.h"
#include "Network/ReplicationTypes.h"
#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Settings/SettingsRegistry.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Editor/ConsoleVariableWidgets.h"
#include "Editor/EditorActions.h"
#include "Editor/EditorCameraState.h"
#include "Editor/EditorPreferences.h"
#include "Editor/NavMeshBaker.h"
#include "AI/AIComponents.h"
#include "Editor/EditorTheme.h"
#include "Editor/SceneEditOps.h"
#include "Editor/TerrainDemoGenerator.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/ModelImportSettings.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimGraph.h"
#include "Scene/Prefab.h"
#include "Scene/Sequence.h"
#include "Scene/SceneSerializer.h"
#include "UI/UIReflection.h"
#include "UI/UISystem.h"
#include "World/GameWorldTravel.h"

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
	FEditorPreferences::Get().Initialize(); // 개인 환경설정 (%LOCALAPPDATA%/ProjectE/EditorPreferences, <Saved>/Config)
	RegisterAudioTypes(); // 씬 로드 전에 (인스펙터/직렬화)
	RegisterPhysicsTypes();
	RegisterAITypes();
	RegisterNetworkTypes();
	RegisterUITypes();
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
	Context.ChangePrefab       = [this](const std::function<bool()>& Change) { return ChangePrefabAsset(Change); };
	FPrefabLibrary::Get().SetContentDirectory(Context.ContentDirectory); // 씬 로드(인스턴스 동기화) 전에
	Context.Scripts          = &Scripts;
	Context.AI               = &World.GetAI();
	// 게임 월드: 스크립트 콘텐츠 경로와 물리 훅도 연결한다
	World.Init({ &Scripts, &Physics, &GameModule, &Resources, Context.ContentDirectory });
	Scripts.SetAudioHooks({
		[this](FEntity Entity) { AudioSystem.Play(Audio, Entity); },
		[this](FEntity Entity) { AudioSystem.Stop(Audio, Entity); },
		[this](const std::string& ClipAsset) { Audio.PlayOneShot(Scripts.GetContentDirectory() / FStringConv::ToWide(ClipAsset)); },
	});
	// Lua Game.Quit()은 에디터에서 플레이 정지 (창 모드/VSync는 에디터 창에 적용하지 않는다)
	Scripts.SetAppHooks({ [this]() { bScriptStopPlayRequested = true; } });
	if (Audio.Init() && IsAutomationRun())
	{
		Audio.SetMasterVolume(0.0f); // 자동 검증 중에는 소리를 내지 않는다
	}
	PlayMode.Init(Scene, World);
	// 지형 도구: 뷰포트 브러시 (모드가 켜져 있을 때만 마우스를 가져간다)
	FTerrainLibrary::Get().SetContentDirectory(Context.ContentDirectory);
	FFoliageLibrary::Get().SetContentDirectory(Context.ContentDirectory);
	ViewportPanel.ToolOverlay = [this](FEditorContext& InContext, const FInput& InInput, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered) {
		// 지형/폴리지 도구 중 켜진 하나만 (둘 다 켜면 지형 우선)
		const bool bTerrain = TerrainToolPanel.HandleViewport(InContext, InInput, ImageMin, ImageSize, bHovered);
		return TerrainToolPanel.IsActive() ? bTerrain : FoliageToolPanel.HandleViewport(InContext, InInput, ImageMin, ImageSize, bHovered);
	};
	NetPlay.Init(World, &Resources, Context.ContentDirectory);
	Context.NetPlay = &NetPlay;
	// 콘솔 stat fps/gpu: 뷰포트 오른쪽 위 (표 줄은 '\t'로 열 구분)
	ViewportPanel.StatOverlay = [this](const FVector2& ImageMin, const FVector2& ImageSize) {
		const std::vector<std::string> Lines = StatOverlay.BuildLines(&SceneRenderer.GetStats());
		if (Lines.empty())
		{
			return;
		}
		const float FontSize      = ImGui::GetFontSize();
		const float Columns[]     = { FontSize * 9.0f, FontSize * 4.5f };
		const float LineHeight    = ImGui::GetTextLineHeightWithSpacing();
		float       Width         = 0.0f;
		for (const std::string& Line : Lines)
		{
			const size_t Tab = Line.find('\t');
			Width = std::max(Width, Tab == std::string::npos ? ImGui::CalcTextSize(Line.c_str()).x : Columns[0] + Columns[1] + FontSize * 4.0f);
		}
		ImDrawList*  DrawList = ImGui::GetWindowDrawList();
		const ImVec2 Min(ImageMin.X + ImageSize.X - Width - 20.0f, ImageMin.Y + 44.0f);
		DrawList->AddRectFilled(Min, ImVec2(Min.x + Width + 12.0f, Min.y + LineHeight * static_cast<float>(Lines.size()) + 8.0f), IM_COL32(0, 0, 0, 150), 4.0f);
		float Y = Min.y + 4.0f;
		for (const std::string& Line : Lines)
		{
			float  X     = Min.x + 6.0f;
			size_t Begin = 0;
			for (size_t Column = 0;; ++Column)
			{
				const size_t End = Line.find('\t', Begin);
				const std::string Cell = Line.substr(Begin, End == std::string::npos ? std::string::npos : End - Begin);
				DrawList->AddText(ImVec2(X, Y), IM_COL32(215, 255, 215, 255), Cell.c_str());
				if (End == std::string::npos)
				{
					break;
				}
				X += Columns[std::min<size_t>(Column, 1)];
				Begin = End + 1;
			}
			Y += LineHeight;
		}
	};

	// --scene <Content 기준 경로>: 시작 씬 지정 (데모/자동 검증). 없거나 실패하면 프로젝트 기본 씬
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	if (CommandLine.HasFlag(L"--generate-terrain-demo"))
	{
		GenerateTerrainDemo(Context.ContentDirectory); // 지형 데모 에셋 다시 만들기 (Terrain/, Scenes/Demo_Terrain.escene)
	}
	if (CommandLine.HasFlag(L"--generate-showcase-terrain"))
	{
		GenerateShowcaseTerrain(Context.ContentDirectory); // 쇼케이스 지형/폴리지 다시 만들기 (씬은 그대로)
	}
	if (const std::wstring SceneArg = CommandLine.GetValue(L"--scene"); SceneArg.empty() || !OpenScene(Context.ContentDirectory / SceneArg))
	{
		OpenStartupScene();
	}
	// 자동 검증: --console-input <글자> 출력 로그 콘솔 줄에 넣고 포커스 (자동 완성 팝업 확인)
	if (const std::wstring ConsoleText = CommandLine.GetValue(L"--console-input"); !ConsoleText.empty())
	{
		OutputLogPanel.FocusConsole(FStringConv::ToUtf8(ConsoleText));
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
	if (const std::wstring Pan = FCommandLine::FromProcess().GetValue(L"--verify-camera-pan"); !Pan.empty())
	{
		VerifyCameraPanPerFrame = std::stof(Pan);
	}
	if (const std::wstring PanStart = FCommandLine::FromProcess().GetValue(L"--verify-camera-pan-start"); !PanStart.empty())
	{
		VerifyCameraPanStart = static_cast<uint64>(std::stoull(PanStart));
	}
	// 자동 검증: --open-settings project|editor [--settings-section <Id>] 으로 설정 창 열기
	if (const std::wstring SettingsArg = FCommandLine::FromProcess().GetValue(L"--open-settings"); !SettingsArg.empty())
	{
		const std::string Section = FStringConv::ToUtf8(FCommandLine::FromProcess().GetValue(L"--settings-section"));
		(SettingsArg == L"editor" ? EditorPreferencesWindow : ProjectSettingsWindow).Open(Section);
	}

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

	// 자동 검증: --bake-navmesh 시작 씬 내비메시 굽기(저장), --show-navmesh 뷰포트 내비메시 표시
	if (FCommandLine::FromProcess().HasFlag(L"--bake-navmesh"))
	{
		BakeNavMesh();
	}
	if (FCommandLine::FromProcess().HasFlag(L"--show-navmesh"))
	{
		ViewportPanel.bShowNavMesh = true;
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

	const FEditorViewportSettings& ViewportPrefs = FEditorPreferences::Get().Viewport;
	Camera.SetPerspective(ViewportPrefs.FieldOfView, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 10.0f, 100000.0f); // cm: 근평면 10cm, 원평면 1km
	Camera.SetPosition(FVector3(-600.0f, -400.0f, 300.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 80.0f));
	CameraController.MoveSpeed = ViewportPrefs.DefaultCameraSpeed; // 저장된 편집 카메라가 있으면 아래에서 덮인다
	ApplyViewportPreferences();
	LoadEditorCamera();
	if (FSettingsSection* Section = FSettingsRegistry::Get().Find("EditorViewport"))
	{
		Section->OnChanged = [this]() { ApplyViewportPreferences(); }; // 설정 창에서 바꾸면 바로
	}

	// 스크립트 핫 리로드: 프로젝트 Content의 .lua 감시
	if (!ScriptWatcher.Start(Context.ContentDirectory, true))
	{
		E_LOG(LogEditor, Warning, "Content 디렉터리 감시를 시작하지 못했습니다. 스크립트 핫 리로드가 꺼집니다");
	}

	// 자동 검증: --terrain-brush-test 지형 스컬프트/칠하기 스트로크 → 실행 취소/다시 실행으로 높이·가중치가 맞는지 (카메라 복원 뒤 — 검증이 시점을 정한다)
	if (CommandLine.HasFlag(L"--terrain-brush-test"))
	{
		VerifyTerrainBrush();
	}
	if (CommandLine.HasFlag(L"--foliage-brush-test"))
	{
		VerifyFoliageBrush();
	}

	E_LOG(LogEditor, Display, "에디터 초기화 완료. 뷰포트: 우클릭 + WASD/QE 시점, 좌클릭 선택, W/E/R 기즈모(Alt+드래그 복제), End 바닥에 붙이기, Ctrl+C/V/D/Z, Ctrl+N/O/S 씬 파일, F5 재생/정지");

	// 자동 검증: --play-net listen|dedicated [--play-clients N] 로 네트워크 플레이 설정 (--play와 함께)
	if (const std::wstring PlayNet = CommandLine.GetValue(L"--play-net"); !PlayNet.empty())
	{
		NetPlay.PendingSettings.Mode = PlayNet == L"dedicated" ? FPlayNetSettings::EMode::DedicatedServer : FPlayNetSettings::EMode::ListenServer;
		if (const std::wstring Clients = CommandLine.GetValue(L"--play-clients"); !Clients.empty())
		{
			NetPlay.PendingSettings.ClientCount = std::clamp(std::stoi(Clients), 0, 4);
		}
		NetPlay.PendingSettings.Port = FNetLaunchOptions::FromCommandLine(CommandLine).Port;
	}
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
	UpdateAutoSave(DeltaSeconds);

	// (게임 UI 텍스트 상자에 입력 중이면 ESC는 UI가 받는다 — 직전 프레임 기준)
	if (InputState.IsKeyPressed(EKey::Escape) && !ImGuiLayer.WantCaptureKeyboard() && !ViewportPanel.bGameUIWantsKeyboard)
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
	if (VerifyCameraPanPerFrame != 0.0f && Context.Camera == &Camera && GetFrameIndex() >= VerifyCameraPanStart)
	{
		Camera.SetPosition(Camera.GetPosition() + Camera.GetRightVector() * VerifyCameraPanPerFrame);
	}

	FEditorActions::PruneSelection(Context);
	SyncNavMeshDisplay();

	UpdatePlayMode(DeltaSeconds);
	// 애니메이션/파티클은 편집 중에도 재생해 보여준다 (플레이 중이면 플레이 씬)
	World.TickPresentation(*Context.Scene, DeltaSeconds);
	AssetEditors.Update(Context, DeltaSeconds);

	PollShaderChanges();
	PollScriptChanges();

	StatOverlay.Tick(DeltaSeconds);
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
	TerrainToolPanel.Update(Context); // Undo/Redo로 바뀐 지형/폴리지 편집 버전 맞추기
	FoliageToolPanel.Update(Context);
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
	TerrainToolPanel.Draw(Context);
	FoliageToolPanel.Draw(Context);
	ProjectSettingsWindow.Draw(Context);
	EditorPreferencesWindow.Draw(Context);
	OutputLogPanel.Draw(Context);
	NetworkPanel.Draw(Context);
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
	if (VerifyCameraPanPerFrame == 0.0f) // 검증용으로 민 카메라는 저장하지 않는다 (다음 실행 시점이 밀림)
	{
		SaveEditorCamera();
	}
	// 뷰포트 툴바에서 바꾼 스냅 값을 개인 환경설정에 (자동 검증은 개인 설정을 쓰지 않는다)
	if (!IsAutomationRun())
	{
		FEditorViewportSettings& ViewportPrefs = FEditorPreferences::Get().Viewport;
		ViewportPrefs.bSnapEnabled             = ViewportPanel.Snap.bEnabled;
		ViewportPrefs.TranslateSnap            = ViewportPanel.Snap.TranslateStep;
		ViewportPrefs.RotateSnap               = ViewportPanel.Snap.RotateStepDegree;
		ViewportPrefs.ScaleSnap                = ViewportPanel.Snap.ScaleStep;
		FEditorPreferences::Get().SaveViewport();
	}
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
	AssetEditors.EndScenePreviews(Context); // 시퀀서 미리보기 값을 되돌린 뒤 씬을 바꾼다
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
	AssetEditors.EndScenePreviews(Context);
	Context.ClearSelection();
	if (!FSceneSerializer::LoadFromFile(Scene, Path))
	{
		return false;
	}
	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
	CurrentScenePath = Path;
	ResetUndoHistory();
	RememberOpenedScene();
	return true;
}

bool FEditorApplication::SaveScene()
{
	if (CurrentScenePath.empty())
	{
		return SaveSceneAs();
	}
	FPrefabLibrary::Get().RecordAllOverrides(Scene); // 아직 커밋 안 된 인스턴스 편집도 오버라이드로
	AssetEditors.SwapScenePreviews(Context);         // 시퀀서 미리보기 값이 아니라 원래 값을 저장
	const bool bSaved = FSceneSerializer::SaveToFile(Scene, CurrentScenePath);
	AssetEditors.SwapScenePreviews(Context);
	if (!bSaved)
	{
		return false;
	}
	FTerrainLibrary::Get().SaveAllUnsaved(); // 씬이 가리키는 지형/폴리지 데이터(.eterrain/.efoliage)도 함께
	FFoliageLibrary::Get().SaveAllUnsaved();
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
	FPrefabLibrary::Get().RecordAllOverrides(Scene);
	AssetEditors.SwapScenePreviews(Context);
	const bool bSaved = FSceneSerializer::SaveToFile(Scene, Path);
	AssetEditors.SwapScenePreviews(Context);
	if (!bSaved)
	{
		return false;
	}
	CurrentScenePath = Path;
	FTerrainLibrary::Get().SaveAllUnsaved(); // 씬이 가리키는 지형/폴리지 데이터(.eterrain/.efoliage)도 함께
	FFoliageLibrary::Get().SaveAllUnsaved();
	UndoHistory.MarkSaved();
	UpdateWindowTitle();
	RememberOpenedScene();
	return true;
}

void FEditorApplication::RememberOpenedScene()
{
	if (CurrentScenePath.empty() || IsAutomationRun())
	{
		return;
	}
	std::error_code             ErrorCode;
	const std::filesystem::path Relative = std::filesystem::relative(CurrentScenePath, Context.ContentDirectory, ErrorCode);
	if (ErrorCode || Relative.empty() || *Relative.begin() == L"..")
	{
		return; // Content 밖 씬은 기억하지 않는다
	}
	FEditorProjectState& State = FEditorPreferences::Get().ProjectState;
	const std::string    SceneAsset = FStringConv::ToUtf8(Relative.generic_wstring());
	if (State.LastOpenedScene != SceneAsset)
	{
		State.LastOpenedScene = SceneAsset;
		FEditorPreferences::Get().SaveProjectState();
	}
}

void FEditorApplication::ApplyViewportPreferences()
{
	const FEditorViewportSettings& Prefs = FEditorPreferences::Get().Viewport;
	CameraController.LookSensitivity     = Prefs.MouseSensitivity;
	if (!Camera.IsOrthographic())
	{
		Camera.SetPerspective(Prefs.FieldOfView, Camera.GetAspectRatio(), Camera.GetNearZ(), Camera.GetFarZ());
	}
	ViewportPanel.Snap.bEnabled         = Prefs.bSnapEnabled;
	ViewportPanel.Snap.TranslateStep    = Prefs.TranslateSnap;
	ViewportPanel.Snap.RotateStepDegree = Prefs.RotateSnap;
	ViewportPanel.Snap.ScaleStep        = Prefs.ScaleSnap;
	if (Rhi)
	{
		Rhi->SetVSync(Prefs.bVSync);
	}
}

void FEditorApplication::UpdateAutoSave(float DeltaSeconds)
{
	const FEditorGeneralSettings& Prefs = FEditorPreferences::Get().General;
	if (!Prefs.bAutoSave || PlayMode.IsActive() || IsAutomationRun())
	{
		AutoSaveElapsedSeconds = 0.0f;
		return;
	}
	AutoSaveElapsedSeconds += DeltaSeconds;
	if (AutoSaveElapsedSeconds < std::max(Prefs.AutoSaveIntervalMinutes, 1.0f) * 60.0f)
	{
		return;
	}
	AutoSaveElapsedSeconds = 0.0f;
	if (!UndoHistory.IsDirty())
	{
		return;
	}

	// <Saved>/Autosaves/<씬 이름>_<시각>.escene — 원본 씬 파일은 건드리지 않는다
	const std::wstring          Stem      = CurrentScenePath.empty() ? std::wstring(L"Untitled") : CurrentScenePath.stem().wstring();
	const std::filesystem::path Directory = FPaths::GetSavedDirectory() / L"Autosaves";
	SYSTEMTIME Time;
	GetLocalTime(&Time);
	const std::filesystem::path Path = Directory / std::format(L"{}_{:04}{:02}{:02}_{:02}{:02}{:02}.escene", Stem, Time.wYear, Time.wMonth, Time.wDay,
	                                                           Time.wHour, Time.wMinute, Time.wSecond);
	AssetEditors.SwapScenePreviews(Context);
	const bool bSaved = FSceneSerializer::SaveToFile(Scene, Path);
	AssetEditors.SwapScenePreviews(Context);
	if (!bSaved)
	{
		return;
	}
	E_LOG(LogEditor, Display, "자동 저장: {}", FStringConv::ToUtf8(Path.wstring()));

	// 이 씬의 오래된 사본 정리 (이름 = 시각이라 이름순 = 시간순)
	std::vector<std::filesystem::path> Copies;
	std::error_code                    ErrorCode;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Directory, ErrorCode))
	{
		const std::wstring Name = Entry.path().filename().wstring();
		if (Entry.path().extension() == L".escene" && Name.size() == Stem.size() + 23 && Name.compare(0, Stem.size() + 1, Stem + L"_") == 0)
		{
			Copies.push_back(Entry.path());
		}
	}
	std::sort(Copies.begin(), Copies.end());
	const size_t Keep = std::max<size_t>(Prefs.AutoSaveKeepCount, 1);
	for (size_t Index = 0; Index + Keep < Copies.size(); ++Index)
	{
		std::filesystem::remove(Copies[Index], ErrorCode);
	}
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

	// 개인 환경설정: 이 프로젝트에서 마지막으로 연 씬 (자동 검증은 개인 상태를 쓰지 않는다)
	const std::string& LastScene = FEditorPreferences::Get().ProjectState.LastOpenedScene;
	if (FEditorPreferences::Get().General.bLoadLastSceneOnStartup && !LastScene.empty() && !IsAutomationRun())
	{
		const std::filesystem::path LastPath = Context.ContentDirectory / FStringConv::ToWide(LastScene);
		if (std::filesystem::exists(LastPath) && OpenScene(LastPath))
		{
			return;
		}
	}

	// 프로젝트 설정 "에디터 시작 맵" (비면 게임 기본 맵)
	const std::string StartupMap = FPaths::HasProject() ? FProjectSettings::Get().GetEditorStartupMap() : std::string();
	if (!StartupMap.empty())
	{
		const std::filesystem::path ScenePath = Context.ContentDirectory / FStringConv::ToWide(StartupMap);
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
				E_LOG(LogEditor, Display, "기본 씬 생성: {}", StartupMap);
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

	// 선택 대상 편집 (뷰포트/계층 창에 포커스가 있을 때만 — 콘텐츠 브라우저 등은 자체 Delete/복사를 쓴다).
	// ImGui 키보드 내비게이션이 켜져 있어 창 포커스 중에는 WantCaptureKeyboard가 항상 참이므로 창 포커스로 판정한다
	if (!ViewportPanel.IsFocused() && !HierarchyPanel.IsFocused())
	{
		return;
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
	{
		FEditorActions::CopySelection(Context);
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
	{
		FEditorActions::PasteClipboard(Context);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		FEditorActions::DeleteSelection(Context);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_End, false))
	{
		FEditorActions::SnapSelectionToFloor(Context);
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
		ImGui::MenuItem("지형", nullptr, &TerrainToolPanel.bOpen);
		ImGui::MenuItem("폴리지", nullptr, &FoliageToolPanel.bOpen);
		ImGui::MenuItem("네트워크", nullptr, &NetworkPanel.bOpen);
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
		ImGui::Text("메시: %u / %u 표시, 드로우 %u (그림자 %u)", Stats.VisibleMeshes, Stats.TotalMeshes, Stats.DrawCalls, Stats.ShadowDrawCalls);
		ImGui::Text("삼각형: %llu (그림자 %llu)", static_cast<unsigned long long>(Stats.Triangles), static_cast<unsigned long long>(Stats.ShadowTriangles));
		if (ImGui::BeginTable("RenderTimers", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("구간");
			ImGui::TableSetupColumn("CPU ms");
			ImGui::TableSetupColumn("GPU ms");
			ImGui::TableHeadersRow();
			for (uint32 Index = 0; Index < static_cast<uint32>(ERenderTimer::Count); ++Index)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(GetRenderTimerName(static_cast<ERenderTimer>(Index)));
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", Stats.CpuMs[Index]);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", Stats.GpuMs[Index]);
			}
			ImGui::EndTable();
		}
		ImGui::Text("파티클: %u (화면 밖 이미터 %u)", Stats.Particles, Stats.ParticleEmittersCulled);
		ImGui::Text("점광원/스포트: %u (그림자 %u장)", Stats.LocalLights, Stats.LocalShadowSlices);
		ImGui::Text("스킨 메시: %u 그림 (가시성 제외 %u)", Stats.SkinnedDrawn, Stats.SkinnedCulled);
		{
			const FD3D12DynamicUploadBuffer& Upload = Rhi->GetDynamicBuffer();
			constexpr double                 Mb     = 1024.0 * 1024.0;
			ImGui::Text("업로드 버퍼: 지난 프레임 %.2f / %.1f MB (씬 렌더러 %.2f MB, 확장 %u회)", static_cast<double>(Upload.GetLastFrameUsed()) / Mb,
			            static_cast<double>(Upload.GetCapacity()) / Mb, static_cast<double>(Stats.UploadBytes) / Mb, Upload.GetGrowCount());
		}
		ImGui::Text("엔티티: %u", Context.Scene->GetRegistry().GetAliveCount());
		ImGui::Text("리소스: 메시 %zu, 머티리얼 %zu, 텍스처 %zu", Resources.GetMeshCount(), Resources.GetMaterialCount(),
		            Resources.GetTextureCount());

		bool bVSync = Rhi->IsVSync();
		if (ImGui::Checkbox("VSync", &bVSync))
		{
			// 환경설정(뷰포트)에 저장해 다음 실행에도 유지 — 자동 검증 실행은 개인 설정을 쓰지 않는다
			Rhi->SetVSync(bVSync);
			FEditorPreferences::Get().Viewport.bVSync = bVSync;
			if (FSettingsSection* Section = FSettingsRegistry::Get().Find("EditorViewport"); Section != nullptr && !IsAutomationRun())
			{
				Section->Save();
			}
		}
		// 렌더 토글은 콘솔 변수 (r.* — 콘솔/명령줄과 같은 값, 렌더러가 매 프레임 읽는다)
		ConsoleVariableWidgets::Checkbox("오클루전 컬링", "r.Occlusion");
		if (ConsoleVariableWidgets::GetBool("r.Occlusion"))
		{
			ImGui::SameLine();
			ImGui::Text("정적 %u 중 그림 %u (+2단계 %u), 가려짐 %u", Stats.OcclusionTested, Stats.OcclusionPhase1, Stats.OcclusionPhase2,
			            Stats.OcclusionTested - std::min(Stats.OcclusionTested, Stats.OcclusionPhase1 + Stats.OcclusionPhase2));
		}
		ConsoleVariableWidgets::Checkbox("스킨 가시성 컬링", "r.SkinCulling");
		ConsoleVariableWidgets::Checkbox("메시 LOD", "r.LOD");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		ImGui::SliderFloat("LOD 배율", &SceneRenderer.LodScale, 0.25f, 4.0f, "%.2f");
		ImGui::SetNextItemWidth(120.0f);
		ConsoleVariableWidgets::SliderFloat("LOD 전환 여유", "r.LODHysteresis", 0.0f, 0.5f);
		ConsoleVariableWidgets::Checkbox("깊이 사전 패스", "r.DepthPrepass");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		ConsoleVariableWidgets::Combo("버퍼 확인", "r.DebugView");
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
		if (ImGui::MenuItem(ICON_FA_ROUTE " 내비메시 굽기", nullptr, false, !PlayMode.IsActive()))
		{
			BakeNavMesh();
		}
		ImGui::SetItemTooltip("씬의 정적 메시로 내비메시를 굽고 씬 옆 .enav로 저장합니다 (설정: NavMeshComponent)");
		if (ImGui::MenuItem(ICON_FA_CAMERA " 반사 캡처 굽기", nullptr, false, !PlayMode.IsActive()))
		{
			SceneRenderer.RequestReflectionCaptureBake();
			ShowNotification("반사 캡처를 굽습니다 (경로가 비어 있던 캡처는 Captures/<이름>.ecapture — 씬 저장 필요)", false);
		}
		ImGui::SetItemTooltip("반사 캡처 컴포넌트마다 주변을 큐브맵으로 그려 .ecapture로 저장합니다");
		ImGui::TextDisabled(ShaderWatcher.IsWatching() ? "셰이더 자동 감시: 켜짐" : "셰이더 자동 감시: 꺼짐");
		ImGui::EndMenu();
	}
}

void FEditorApplication::BakeNavMesh()
{
	if (PlayMode.IsActive())
	{
		ShowNotification("플레이 중에는 내비메시를 구울 수 없습니다", true);
		return;
	}
	Scene.UpdateTransforms();
	FNavMesh                     NavMesh;
	const FNavMeshBaker::FResult Result = FNavMeshBaker::Bake(Scene, Resources, NavMesh);
	if (!Result.bSucceeded)
	{
		ShowNotification("내비메시 굽기 실패: " + Result.Error, true);
		return;
	}

	// 씬 옆 <씬 이름>.enav (저장한 적 없는 씬이면 Content/NavMesh.enav)
	const std::filesystem::path File = CurrentScenePath.empty() ? Context.ContentDirectory / L"NavMesh.enav"
	                                                            : std::filesystem::path(CurrentScenePath).replace_extension(FNavMesh::FileExtension);
	if (!NavMesh.SaveToFile(File))
	{
		ShowNotification("내비메시 파일을 저장하지 못했습니다: " + FStringConv::ToUtf8(File.wstring()), true);
		return;
	}

	// 씬의 내비메시 컴포넌트가 이 파일을 가리키게 한다 (없으면 만든다)
	FRegistry& Registry = Scene.GetRegistry();
	FEntity    Owner;
	Registry.View<FNavMeshComponent>().Each([&](FEntity Entity, FNavMeshComponent&) {
		if (!Owner.IsValid())
		{
			Owner = Entity;
		}
	});
	if (!Owner.IsValid())
	{
		Owner = Scene.CreateEntity("NavMesh");
		Registry.Emplace<FNavMeshComponent>(Owner);
	}
	Registry.Get<FNavMeshComponent>(Owner).NavMeshAsset = FModelLoader::MakeAssetPath(File);
	Context.MarkEdited("내비메시 굽기");

	std::vector<FVector3> Triangles;
	NavMesh.GetDebugTriangles(Triangles);
	ViewportPanel.SetNavMeshTriangles(std::move(Triangles));
	ViewportPanel.bShowNavMesh = true;
	DisplayedNavMeshAsset      = Registry.Get<FNavMeshComponent>(Owner).NavMeshAsset;
	E_LOG(LogEditor, Display, "내비메시 굽기: 메시 {}개, 삼각형 {}개 → 폴리곤 {}개 ({})", Result.MeshCount, Result.TriangleCount, Result.PolygonCount,
	      DisplayedNavMeshAsset);
	ShowNotification(std::format("내비메시 굽기 완료: 폴리곤 {}개 (메시 {}개) → {}", Result.PolygonCount, Result.MeshCount, DisplayedNavMeshAsset), false);
}

void FEditorApplication::SyncNavMeshDisplay()
{
	// 편집 씬의 내비메시 파일이 바뀌면(씬 열기, 실행 취소, 굽기) 뷰포트 표시를 다시 읽는다
	std::string Asset;
	Scene.GetRegistry().View<FNavMeshComponent>().Each([&](FEntity, FNavMeshComponent& Component) {
		if (Asset.empty())
		{
			Asset = Component.NavMeshAsset;
		}
	});
	if (Asset == DisplayedNavMeshAsset)
	{
		return;
	}
	DisplayedNavMeshAsset = Asset;
	std::vector<FVector3> Triangles;
	FNavMesh              NavMesh;
	if (!Asset.empty() && NavMesh.LoadFromFile(Context.ContentDirectory / FStringConv::ToWide(Asset)))
	{
		NavMesh.GetDebugTriangles(Triangles);
	}
	ViewportPanel.SetNavMeshTriangles(std::move(Triangles));
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
	// 지형/폴리지: 씬 기록과 함께 편집 기록을 비우고 데이터를 파일에서 다시 읽는다 (저장 안 한 편집은 버리고, 따로 저장한 데이터는 그대로)
	TerrainToolPanel.GetHistory().Clear();
	FoliageToolPanel.GetHistory().Clear();
	FTerrainLibrary::Get().Clear();
	FFoliageLibrary::Get().Clear();
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
	// 프리팹 인스턴스에서 원본과 달라진 항목을 오버라이드로 기록한 뒤 스냅샷 (인스턴스가 현재 원본에 맞춰져 있다는 전제 — ChangePrefabAsset 참고)
	// 시퀀서 미리보기 중이면 미리보기 값 대신 원래 값으로 기록 (맞바꿨다가 되돌린다)
	AssetEditors.SwapScenePreviews(Context);
	FPrefabLibrary::Get().RecordAllOverrides(Scene);
	std::string Snapshot = FSceneSerializer::ToJsonString(Scene);
	AssetEditors.SwapScenePreviews(Context);
	if (UndoHistory.Commit(std::move(Label), std::move(Snapshot)))
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
	if (ImGui::MenuItem("복사", "Ctrl+C", false, bHasSelection))
	{
		FEditorActions::CopySelection(Context);
	}
	if (ImGui::MenuItem("붙여넣기", "Ctrl+V", false, !Context.EntityClipboard.empty()))
	{
		FEditorActions::PasteClipboard(Context);
	}
	if (ImGui::MenuItem("복제", "Ctrl+D / Alt+드래그", false, bHasSelection))
	{
		FEditorActions::DuplicateSelection(Context);
	}
	if (ImGui::MenuItem("삭제", "Del", false, bHasSelection))
	{
		FEditorActions::DeleteSelection(Context);
	}
	if (ImGui::MenuItem("바닥에 붙이기", "End", false, bHasSelection))
	{
		FEditorActions::SnapSelectionToFloor(Context);
	}
	if (ImGui::MenuItem("선택 항목 포커스", "F", false, bHasSelection))
	{
		ViewportPanel.FocusSelection(Context);
	}
	ImGui::Separator();
	ImGui::MenuItem("그리드 표시", nullptr, &ViewportPanel.bShowGrid);
	ImGui::MenuItem("기즈모 스냅", nullptr, &ViewportPanel.Snap.bEnabled);
	ImGui::Separator();
	if (ImGui::MenuItem(ICON_FA_SLIDERS " 프로젝트 설정..."))
	{
		ProjectSettingsWindow.Open();
	}
	if (ImGui::MenuItem(ICON_FA_GEAR " 에디터 환경설정..."))
	{
		EditorPreferencesWindow.Open();
	}
	ImGui::Separator();
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
	if (State.bOrthographic)
	{
		Camera.SetOrthographic(State.OrthoHeight, Camera.GetAspectRatio(), Camera.GetNearZ(), Camera.GetFarZ());
	}
	E_LOG(LogEditor, Log, "에디터 카메라 복원: ({:.0f}, {:.0f}, {:.0f})", State.Position.X, State.Position.Y, State.Position.Z);
}

void FEditorApplication::SaveEditorCamera() const
{
	FEditorCameraState State;
	State.Position  = Camera.GetPosition();
	State.Rotation  = Camera.GetRotation();
	State.MoveSpeed     = CameraController.MoveSpeed;
	State.bOrthographic = Camera.IsOrthographic();
	State.OrthoHeight   = Camera.GetOrthoHeight();
	State.Save(GetEditorCameraPath());
}

// ---------------------------------------------------------------- 플레이 모드 / 스크립트

void FEditorApplication::StartPlay()
{
	if (PlayMode.IsActive())
	{
		return;
	}
	AssetEditors.EndScenePreviews(Context); // 플레이 씬은 편집 씬 복제 — 시퀀서 미리보기 값을 먼저 되돌린다
	FPlayOptions Options;
	if (NetPlay.Prepare(NetPlay.PendingSettings, Scene, Options))
	{
		PlayMode.Play(Context, Options); // 네트워크 플레이: PIE 씬 파일에서 플레이 씬을 만든다
		NetPlay.AfterPlay();
	}
	else
	{
		PlayMode.Play(Context);
	}
	// Game.GetCurrentScene: 편집 중인 씬 파일 (Content 기준, 저장 안 한 씬이면 "")
	World.SetCurrentSceneAsset(CurrentScenePath.empty() ? std::string() : FPrefabLibrary::Get().MakeAssetPath(CurrentScenePath));
	ShowNotification("플레이 시작 — F5/ESC 정지, F6 일시정지, F7 한 프레임", false);
}

void FEditorApplication::StopPlay()
{
	if (!PlayMode.IsActive())
	{
		return;
	}
	PlayMode.Stop(Context);
	NetPlay.Stop(); // 네트워크 종료 + 서버/클라이언트 창 닫기
	AudioSystem.Reset(Audio);
	// 플레이 중 뷰포트 크기가 바뀌었을 수 있으므로 에디터 카메라 종횡비를 맞춘다
	Context.Camera = &Camera;
	Camera.SetAspectRatio(ViewportPanel.GetAspectRatio(Camera.GetAspectRatio()));
}

void FEditorApplication::UpdatePlayMode(float DeltaSeconds)
{
	// 텍스트 입력 중에는 게임에 키 입력을 주지 않는다
	const bool    bGameInput = !ImGui::GetIO().WantTextInput;
	const FInput* GameInput  = bGameInput ? &GetInput() : nullptr;

	// 게임 UI가 먼저 입력을 본다 (뷰포트 이미지 위 포인터만). 포인터를 가져가면 게임에는 마우스 버튼/휠을 뺀 입력
	NetPlay.Update(DeltaSeconds); // 네트워크 플레이: 수신/클라이언트 보간 (게임플레이 틱 전)
	FInput BlockedInput;
	ViewportPanel.bGameUIWantsPointer  = false;
	ViewportPanel.bGameUIWantsKeyboard = false;
	bool bGameTextInput                = false;
	if (PlayMode.IsActive())
	{
		FUIFrameInput UIInput;
		UIInput.Viewport    = ViewportPanel.GetGameUIViewport();
		UIInput.bHasPointer = GameInput != nullptr;
		if (GameInput != nullptr)
		{
			UIInput.Pointer = FUISystem::MakePointer(*GameInput, -ViewportPanel.GetImageMin(), ViewportPanel.IsHovered());
			UIInput.Keys    = ViewportPanel.IsFocused() ? FUISystem::MakeKeys(*GameInput) : FUIKeyInput{};
		}
		UIInput.DeltaSeconds          = DeltaSeconds;
		const FUIInputResult UIResult = FUISystem::Update(*Context.Scene, UIInput, Context.ContentDirectory);
		// 게임 UI 텍스트 상자 입력 중: IME 조합을 창이 직접 받고 후보 창을 캐럿 아래에 (뷰포트 이미지 위치만큼 옮김)
		bGameTextInput = GameInput != nullptr && UIResult.bKeyboard && UIResult.bHasTextCaret;
		if (bGameTextInput)
		{
			const FVector2 Caret = UIResult.TextCaret.Min + ViewportPanel.GetImageMin();
			GetWindow().SetTextInput(true, static_cast<int32>(Caret.X), static_cast<int32>(Caret.Y), static_cast<int32>(UIResult.TextCaret.GetHeight()));
		}
		if (GameInput != nullptr && (UIResult.bPointer || UIResult.bKeyboard))
		{
			ViewportPanel.bGameUIWantsPointer  = UIResult.bPointer;
			ViewportPanel.bGameUIWantsKeyboard = UIResult.bKeyboard;
			BlockedInput                       = UIResult.bPointer ? GameInput->WithoutMouseButtons() : *GameInput;
			if (UIResult.bKeyboard)
			{
				BlockedInput = BlockedInput.WithoutKeyboard();
			}
			GameInput = &BlockedInput;
		}
	}
	if (!bGameTextInput)
	{
		GetWindow().SetTextInput(false, 0, 0, 0); // 플레이 정지/포커스 해제 → 시스템 IME 처리로 (ImGui 텍스트 필드)
	}
	PlayMode.Tick(Context, DeltaSeconds, GameInput);
	NetPlay.PostTick(DeltaSeconds); // 리슨 서버 복제 전송
	if (bScriptStopPlayRequested)
	{
		bScriptStopPlayRequested = false;
		ShowNotification("Game.Quit() — 플레이를 정지합니다", false);
		StopPlay();
		return;
	}
	if (World.ConsumeSessionRequest())
	{
		ShowNotification("에디터 플레이에서는 Net.Host/Connect/Disconnect를 지원하지 않습니다 (런타임에서 동작)", true);
	}
	// 맵 전환 (Game.OpenScene / 전용 서버의 이동 지시): 플레이 씬만 바꾼다 — 정지하면 편집 씬 복원
	if (PlayMode.IsActive())
	{
		if (const std::optional<std::string> NextScene = FGameWorldTravel::ConsumePending(World, NetPlay.GetActiveDriver()))
		{
			FSceneTravelTargets Targets;
			NetPlay.FillTravelTargets(Targets);
			Targets.OnEndPlay = [this]() { AudioSystem.Reset(Audio); };
			PlayMode.Travel(Context, Targets, *NextScene);
			NetPlay.OnTraveled(*NextScene);
			ShowNotification("맵 전환: " + *NextScene, false);
		}
	}

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
	bool bPrefabChanged = false;
	for (const std::filesystem::path& Path : ScriptWatcher.Poll())
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		bPrefabChanged = bPrefabChanged || Extension == FPrefabLibrary::Extension;
		// 비헤이비어 트리: 플레이 중이면 그 에셋을 쓰는 트리를 새 파일로 다시 시작한다 (편집 창 저장 포함)
		if (Extension == L".ebt")
		{
			World.GetAI().ReloadBehaviorTree(FModelLoader::MakeAssetPath(Path));
			continue;
		}
		// 애니메이션 그래프: 이 파일을 쓰는 컴포넌트가 다음 갱신에서 새 그래프로 다시 묶인다 (파라미터 유지, 편집 중·플레이 중 모두)
		if (Extension == FAnimGraphAsset::Extension)
		{
			FAnimGraphLibrary::Get().Invalidate(FModelLoader::MakeAssetPath(Path));
			continue;
		}
		// 시퀀스: 재생 중인 SequencePlayerComponent가 다음 갱신에서 새 파일을 읽는다
		if (Extension == FSequenceAsset::Extension)
		{
			FSequenceLibrary::Get().Invalidate(FModelLoader::MakeAssetPath(Path));
			continue;
		}
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
	// 프리팹 파일이 바뀌면 (외부 편집, 되돌리기 등) 열린 씬 인스턴스를 다시 맞춘다. 편집 창 저장 직후에도 불리지만 결과는 같다
	if (bPrefabChanged)
	{
		ChangePrefabAsset([] { return true; });
		E_LOG(LogEditor, Log, "프리팹 파일 변경 감지 — 열린 씬 인스턴스 다시 맞춤");
	}
}

bool FEditorApplication::ChangePrefabAsset(const std::function<bool()>& Change)
{
	// 순서가 중요하다: 오버라이드 차이는 "현재 원본"과 비교하므로 원본이 바뀌기 전에 기록하고, 바뀐 뒤에는 인스턴스를 새 원본에 맞춘다
	FPrefabLibrary& Library = FPrefabLibrary::Get();
	Library.RecordAllOverrides(Scene);
	const bool bOk = Change();
	Library.Invalidate();
	Library.SyncAllInstances(Scene);
	FEditorActions::PruneSelection(Context);
	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
	return bOk;
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
	ImGui::DockBuilderDockWindow("###Terrain", Bottom);
	ImGui::DockBuilderDockWindow("###Foliage", Bottom);
	ImGui::DockBuilderDockWindow("###Network", Bottom);
	ImGui::DockBuilderFinish(DockSpace);
}

void FEditorApplication::OnAssetsMoved(const std::vector<FAssetMove>& Moves)
{
	// 1) 리소스 캐시 키 → 새 경로 (같은 에셋을 새 경로로 다시 로드해 중복 생성하지 않게)
	for (const FAssetMove& Move : Moves)
	{
		Resources.OnAssetMoved(Move.From, Move.To);
		FTerrainLibrary::Get().OnAssetMoved(Move.From, Move.To);
		FFoliageLibrary::Get().OnAssetMoved(Move.From, Move.To);
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

	// 프리팹 원본 캐시는 경로가 키이므로 비운다 (다음 사용 시 새 경로로 다시 읽음)
	FPrefabLibrary::Get().Invalidate();

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

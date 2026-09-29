#include "Editor/EditorApplication.h"

#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/SceneSerializer.h"

#include <commdlg.h>
#include <imgui.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

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

	OpenStartupScene();

	// 자동 검증: --select <이름> 으로 시작 시 엔티티 선택 (선택 아웃라인/인스펙터 확인용)
	if (const std::wstring SelectName = FCommandLine::FromProcess().GetValue(L"--select"); !SelectName.empty())
	{
		const std::string Target = FStringConv::ToUtf8(SelectName);
		Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Name) {
			if (!Context.SelectedEntity.IsValid() && Name.Name == Target)
			{
				Context.Select(Entity);
			}
		});
	}

	// 셰이더 핫 리로드: 엔진 셰이더 디렉터리 감시 (실패해도 에디터는 계속)
	if (!ShaderWatcher.Start(FPaths::GetEngineShaderDirectory(), true))
	{
		E_LOG(LogEditor, Warning, "셰이더 디렉터리 감시를 시작하지 못했습니다. Ctrl+R로 수동 다시 로드만 가능합니다");
	}

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 0.1f, 1000.0f);
	Camera.SetPosition(FVector3(-6.0f, -4.0f, 3.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 0.8f));

	E_LOG(LogEditor, Display, "에디터 초기화 완료. 뷰포트: 우클릭 + WASD/QE 시점, 좌클릭 선택, W/E/R 기즈모, Ctrl+N/O/S 씬 파일");
	return true;
}

void FEditorApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();

	if (InputState.IsKeyPressed(EKey::Escape) && !ImGuiLayer.WantCaptureKeyboard())
	{
		RequestExit();
	}

	// 뷰포트 위에서만 카메라 조작 (기즈모 사용 중 제외)
	if (ViewportPanel.IsHovered() && !ViewportPanel.IsUsingGizmo())
	{
		CameraController.Update(Camera, InputState, DeltaSeconds);
	}

	// Delete: 선택 엔티티 삭제 (텍스트 입력 중 제외)
	if (InputState.IsKeyPressed(EKey::Delete) && !ImGuiLayer.WantCaptureKeyboard() && Scene.GetRegistry().IsValid(Context.SelectedEntity))
	{
		Scene.DestroyEntity(Context.SelectedEntity);
		Context.ClearSelection();
	}

	Scene.UpdateTransforms();

	PollShaderChanges();

	const float InstantFps = DeltaSeconds > 0.0f ? 1.0f / DeltaSeconds : 0.0f;
	SmoothedFps            = SmoothedFps <= 0.0f ? InstantFps : FMath::Lerp(SmoothedFps, InstantFps, 0.05f);
}

void FEditorApplication::OnRender()
{
	// 뷰포트 크기 변경은 UI 기술 전에 반영
	ViewportPanel.PrepareFrame(Context);

	ImGuiLayer.BeginFrame();
	HandleShortcuts();
	HandleToolShortcuts();
	DrawMainMenuBar();
	ViewportPanel.Draw(Context, GetInput());
	HierarchyPanel.Draw(Context);
	InspectorPanel.Draw(Context);
	ContentBrowserPanel.Draw(Context);
	PostProcessPanel.Draw(Context);
	ShadowPanel.Draw(Context);
	OutputLogPanel.Draw(Context);
	if (bShowStats)
	{
		DrawStatsWindow();
	}
	if (bShowImGuiDemo)
	{
		ImGui::ShowDemoWindow(&bShowImGuiDemo);
	}
	DrawNotification();

	// 기즈모/인스펙터 편집이 월드 행렬에 즉시 반영되도록 갱신
	Scene.UpdateTransforms();

	const float ClearColor[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
	Rhi->BeginFrame(ClearColor);
	ViewportPanel.RenderScene(Context);

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
	ShaderWatcher.Stop();
	if (Rhi)
	{
		ImGuiLayer.Shutdown(); // 내부에서 GPU Flush
		ViewportPanel.Shutdown();
		SceneRenderer.Shutdown();
		Resources.Shutdown();
		Rhi->Shutdown();
		Rhi.reset();
	}
}

// ---------------------------------------------------------------- 씬 파일

void FEditorApplication::NewScene()
{
	Scene.Clear();
	Context.ClearSelection();
	CurrentScenePath.clear();

	// 빈 씬에도 기본 조명은 둔다
	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 5.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun).Intensity = 3.0f;
	Scene.UpdateTransforms();

	UpdateWindowTitle();
}

bool FEditorApplication::OpenScene(const std::filesystem::path& Path)
{
	Context.ClearSelection();
	if (!FSceneSerializer::LoadFromFile(Scene, Path))
	{
		return false;
	}
	FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
	CurrentScenePath = Path;
	UpdateWindowTitle();
	return true;
}

bool FEditorApplication::SaveScene()
{
	if (CurrentScenePath.empty())
	{
		return SaveSceneAs();
	}
	return FSceneSerializer::SaveToFile(Scene, CurrentScenePath);
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
	UpdateWindowTitle();
	return true;
}

void FEditorApplication::OpenStartupScene()
{
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
			UpdateWindowTitle();
			return;
		}
	}

	BuildDefaultScene();
	UpdateWindowTitle();
}

void FEditorApplication::UpdateWindowTitle()
{
	std::wstring Title = L"ProjectE Editor";
	Title += FPaths::HasProject() ? L" - " + FStringConv::ToWide(FPaths::GetProjectName()) : L" (프로젝트 없음)";
	Title += L" - " + (CurrentScenePath.empty() ? std::wstring(L"제목 없음") : CurrentScenePath.filename().wstring());
	GetWindow().SetTitle(Title);
}

void FEditorApplication::BuildDefaultScene()
{
	Scene.Clear();
	Context.ClearSelection();
	CurrentScenePath.clear();

	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 5.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
	SunLight.Intensity = 3.0f;

	// 바닥: 내장 큐브 + 체커 머티리얼 에셋 (에셋 참조로 기록되어 저장/로드 가능)
	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -0.1f);
	Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
	FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
	GroundMesh.MeshAsset     = "primitive:cube";
	GroundMesh.MaterialAsset = "Materials/Checker.emat";

	// 모델: 루트 엔티티 + 에셋 경로 (자식은 Resolve에서 생성)
	const FEntity Helmet = Scene.CreateEntity("DamagedHelmet");
	Scene.GetTransform(Helmet).Position = FVector3(0.0f, 0.0f, 1.2f);
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
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
	{
		SaveSceneAs();
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
	{
		SaveScene();
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
	if (ImGui::BeginMenu("엔티티"))
	{
		if (ImGui::MenuItem("빈 엔티티 추가"))
		{
			Context.Select(Scene.CreateEntity("Entity"));
		}
		if (ImGui::MenuItem("큐브 추가"))
		{
			const FEntity         Cube = Scene.CreateEntity("Cube");
			FStaticMeshComponent& Mesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Cube);
			Mesh.Mesh      = Context.DefaultCubeMesh;
			Mesh.MeshAsset = "primitive:cube";
			Context.Select(Cube);
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
		ImGui::MenuItem("ImGui 데모", nullptr, &bShowImGuiDemo);
		ImGui::EndMenu();
	}
	DrawToolsMenu();

	ImGui::EndMainMenuBar();
}

void FEditorApplication::DrawStatsWindow()
{
	if (ImGui::Begin("통계", &bShowStats))
	{
		const FSceneRenderStats& Stats = SceneRenderer.GetStats();
		ImGui::Text("FPS: %.1f (%.2f ms)", SmoothedFps, SmoothedFps > 0.0f ? 1000.0f / SmoothedFps : 0.0f);
		ImGui::Text("메시: %u / %u 표시, 드로우 %u", Stats.VisibleMeshes, Stats.TotalMeshes, Stats.DrawCalls);
		ImGui::Text("엔티티: %u", Scene.GetRegistry().GetAliveCount());
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
		ChangedNames += (ChangedNames.empty() ? "" : ", ") + FStringConv::ToUtf8(Path.filename().wstring());
	}
	if (AffectedCount == 0)
	{
		return;
	}

	if (SceneRenderer.ReloadShaders() && ViewportPanel.ReloadShaders(false))
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
	if (SceneRenderer.ReloadShaders(true) && ViewportPanel.ReloadShaders(true))
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

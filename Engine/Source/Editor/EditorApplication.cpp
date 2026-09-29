#include "Editor/EditorApplication.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/PrimitiveShapes.h"

#include <imgui.h>

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
	if (!ImGuiLayer.Init(GetWindow(), *Rhi, std::filesystem::path(E_EDITOR_SAVED_DIR) / L"EditorLayout.ini"))
	{
		return false;
	}

	BuildDefaultScene();

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 0.1f, 1000.0f);
	Camera.SetPosition(FVector3(-6.0f, -4.0f, 3.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 0.8f));

	E_LOG(LogEditor, Display, "에디터 초기화 완료");
	return true;
}

void FEditorApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();

	if (InputState.IsKeyPressed(EKey::Escape) && !ImGuiLayer.WantCaptureKeyboard())
	{
		RequestExit();
	}

	// UI가 마우스를 쓰는 동안은 카메라 조작 중지
	if (!ImGuiLayer.WantCaptureMouse())
	{
		CameraController.Update(Camera, InputState, DeltaSeconds);
	}

	Scene.UpdateTransforms();

	const float InstantFps = DeltaSeconds > 0.0f ? 1.0f / DeltaSeconds : 0.0f;
	SmoothedFps            = SmoothedFps <= 0.0f ? InstantFps : FMath::Lerp(SmoothedFps, InstantFps, 0.05f);
}

void FEditorApplication::OnRender()
{
	// UI 구성 (즉시 모드이므로 렌더 직전에 기술)
	ImGuiLayer.BeginFrame();
	DrawMainMenuBar();
	if (bShowStats)
	{
		DrawStatsWindow();
	}
	if (bShowImGuiDemo)
	{
		ImGui::ShowDemoWindow(&bShowImGuiDemo);
	}

	const float ClearColor[4] = { 0.12f, 0.2f, 0.36f, 1.0f };
	Rhi->BeginFrame(ClearColor);
	SceneRenderer.Render(Scene, Camera);

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
		Camera.SetAspectRatio(static_cast<float>(Width) / static_cast<float>(Height));
	}
}

void FEditorApplication::OnShutdown()
{
	if (Rhi)
	{
		ImGuiLayer.Shutdown();
		SceneRenderer.Shutdown();
		Resources.Shutdown();
		Rhi->Shutdown();
		Rhi.reset();
	}
}

void FEditorApplication::BuildDefaultScene()
{
	const std::filesystem::path ContentDir(E_EDITOR_CONTENT_DIR);

	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
	SunLight.Intensity = 1.2f;

	const FMeshHandle CubeMesh = Resources.CreateMesh(FPrimitiveShapes::MakeCube(1.0f), L"Cube");

	FMaterial GroundMaterial;
	GroundMaterial.Name                    = "Ground";
	GroundMaterial.BaseColorTexture        = Resources.LoadTexture(ContentDir / L"UVChecker.png", true);
	GroundMaterial.Constants.SpecularColor = FVector3(0.2f);
	GroundMaterial.Constants.Shininess     = 32.0f;

	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -0.1f);
	Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
	FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
	GroundMesh.Mesh     = CubeMesh;
	GroundMesh.Material = Resources.CreateMaterial(GroundMaterial);

	const FEntity Helmet = FModelLoader::LoadIntoScene(ContentDir / L"DamagedHelmet.glb", Scene, Resources);
	if (Scene.GetRegistry().IsValid(Helmet))
	{
		Scene.GetTransform(Helmet).Position = FVector3(0.0f, 0.0f, 1.2f);
	}

	Scene.UpdateTransforms();
}

void FEditorApplication::DrawMainMenuBar()
{
	if (!ImGui::BeginMainMenuBar())
	{
		return;
	}

	if (ImGui::BeginMenu("파일"))
	{
		if (ImGui::MenuItem("종료", "ESC"))
		{
			RequestExit();
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("창"))
	{
		ImGui::MenuItem("통계", nullptr, &bShowStats);
		ImGui::MenuItem("ImGui 데모", nullptr, &bShowImGuiDemo);
		ImGui::EndMenu();
	}

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

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

	Context.Rhi              = Rhi.get();
	Context.Resources        = &Resources;
	Context.Renderer         = &SceneRenderer;
	Context.Scene            = &Scene;
	Context.Camera           = &Camera;
	Context.ContentDirectory = std::filesystem::path(E_EDITOR_CONTENT_DIR);
	Context.DefaultCubeMesh  = Resources.CreateMesh(FPrimitiveShapes::MakeCube(1.0f), L"Cube");

	BuildDefaultScene();

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 0.1f, 1000.0f);
	Camera.SetPosition(FVector3(-6.0f, -4.0f, 3.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 0.8f));

	E_LOG(LogEditor, Display, "에디터 초기화 완료. 뷰포트: 우클릭 + WASD/QE 시점, 좌클릭 선택, W/E/R 기즈모");
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

	const float InstantFps = DeltaSeconds > 0.0f ? 1.0f / DeltaSeconds : 0.0f;
	SmoothedFps            = SmoothedFps <= 0.0f ? InstantFps : FMath::Lerp(SmoothedFps, InstantFps, 0.05f);
}

void FEditorApplication::OnRender()
{
	// 뷰포트 크기 변경은 UI 기술 전에 반영
	ViewportPanel.PrepareFrame(Context);

	ImGuiLayer.BeginFrame();
	DrawMainMenuBar();
	ViewportPanel.Draw(Context, GetInput());
	HierarchyPanel.Draw(Context);
	InspectorPanel.Draw(Context);
	ContentBrowserPanel.Draw(Context);
	if (bShowStats)
	{
		DrawStatsWindow();
	}
	if (bShowImGuiDemo)
	{
		ImGui::ShowDemoWindow(&bShowImGuiDemo);
	}

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

void FEditorApplication::BuildDefaultScene()
{
	const std::filesystem::path& ContentDir = Context.ContentDirectory;

	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 5.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
	SunLight.Intensity = 1.2f;

	FMaterial GroundMaterial;
	GroundMaterial.Name                    = "Ground";
	GroundMaterial.BaseColorTexture        = Resources.LoadTexture(ContentDir / L"UVChecker.png", true);
	GroundMaterial.Constants.SpecularColor = FVector3(0.2f);
	GroundMaterial.Constants.Shininess     = 32.0f;

	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -0.1f);
	Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
	FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
	GroundMesh.Mesh     = Context.DefaultCubeMesh;
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
	if (ImGui::BeginMenu("엔티티"))
	{
		if (ImGui::MenuItem("빈 엔티티 추가"))
		{
			Context.Select(Scene.CreateEntity("Entity"));
		}
		if (ImGui::MenuItem("큐브 추가"))
		{
			const FEntity Cube = Scene.CreateEntity("Cube");
			Scene.GetRegistry().Emplace<FStaticMeshComponent>(Cube).Mesh = Context.DefaultCubeMesh;
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
		ImGui::MenuItem("통계", nullptr, &bShowStats);
		ImGui::Separator();
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

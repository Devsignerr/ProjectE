#include "RuntimeApplication.h"

#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/SceneCamera.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/SceneSerializer.h"

E_DEFINE_LOG_CATEGORY(LogRuntime, Log)

namespace
{
	FApplicationDesc MakeRuntimeDesc()
	{
		FApplicationDesc Desc;
		Desc.Window.Title  = L"ProjectE";
		Desc.Window.Width  = 1280;
		Desc.Window.Height = 720;
		return Desc;
	}
} // namespace

FRuntimeApplication::FRuntimeApplication()
	: FApplication(MakeRuntimeDesc())
{
}

FRuntimeApplication::~FRuntimeApplication() = default;

bool FRuntimeApplication::OnInit()
{
	// FApplication::Run이 FPaths를 초기화했으므로 여기서는 프로젝트만 확인
	if (FPaths::HasProject())
	{
		GetWindow().SetTitle(FStringConv::ToWide(FPaths::GetProjectName()));
		E_LOG(LogRuntime, Display, "프로젝트: {}", FPaths::GetProjectName());
	}
	else
	{
		E_LOG(LogRuntime, Warning, "프로젝트가 없습니다 (--project <경로>). 자리표시 씬만 표시합니다");
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

	// 프로젝트 기본 씬(또는 --scene <Content 기준 경로>) 로드, 없거나 실패하면 자리표시 씬
	bool               bSceneLoaded = false;
	const std::wstring SceneArg     = FCommandLine::FromProcess().GetValue(L"--scene");
	if (FPaths::HasProject() && (!SceneArg.empty() || !FPaths::GetProjectDescriptor().DefaultScene.empty()))
	{
		const std::filesystem::path ScenePath = FPaths::GetProjectContentDirectory() /
		                                        (SceneArg.empty() ? FStringConv::ToWide(FPaths::GetProjectDescriptor().DefaultScene) : SceneArg);
		if (FSceneSerializer::LoadFromFile(Scene, ScenePath))
		{
			FSceneAssetResolver::Resolve(Scene, Resources, FPaths::GetProjectContentDirectory());
			bSceneLoaded = true;
			E_LOG(LogRuntime, Display, "씬 로드: {}", FStringConv::ToUtf8(ScenePath.filename().wstring()));
		}
		else
		{
			E_LOG(LogRuntime, Warning, "씬을 열지 못해 자리표시 씬을 표시합니다: {}", FStringConv::ToUtf8(ScenePath.wstring()));
		}
	}
	if (!bSceneLoaded)
	{
		BuildPlaceholderScene();
	}

	Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 10.0f, 100000.0f); // cm: 근평면 10cm, 원평면 1km
	Camera.SetPosition(FVector3(-600.0f, -400.0f, 300.0f));
	Camera.LookAt(FVector3(0.0f, 0.0f, 50.0f));

	// 게임 시작: 스크립트 컴포넌트 실행
	Scripts.SetContentDirectory(FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory());
	Scripts.BeginPlay(Scene);

	E_LOG(LogRuntime, Display, "런타임 초기화 완료 (ESC 종료)");
	return true;
}

void FRuntimeApplication::OnUpdate(float DeltaSeconds)
{
	const FInput& InputState = GetInput();
	if (InputState.IsKeyPressed(EKey::Escape))
	{
		RequestExit();
	}

	Scripts.Update(DeltaSeconds, &InputState);
	if (Scripts.ConsumeSceneStructureChanged())
	{
		FSceneAssetResolver::Resolve(Scene, Resources, Scripts.GetContentDirectory());
	}
	Scene.UpdateTransforms();

	// 주 카메라 컴포넌트가 있으면 그 시점, 없으면 자유 비행 카메라
	const FEntity CameraEntity = FSceneCamera::FindPrimary(Scene);
	if (!CameraEntity.IsValid() || !FSceneCamera::ApplyToCamera(Scene, CameraEntity, Camera.GetAspectRatio(), Camera))
	{
		CameraController.Update(Camera, InputState, DeltaSeconds);
	}
}

void FRuntimeApplication::OnRender()
{
	const float ClearColor[4] = { 0.12f, 0.2f, 0.36f, 1.0f };
	Rhi->BeginFrame(ClearColor);
	SceneRenderer.Render(Scene, Camera, Rhi->GetBackBufferOutput());
	Rhi->EndFrame();
}

void FRuntimeApplication::OnResize(uint32 Width, uint32 Height)
{
	if (Rhi)
	{
		Rhi->Resize(Width, Height);
		Camera.SetAspectRatio(static_cast<float>(Width) / static_cast<float>(Height));
	}
}

void FRuntimeApplication::OnShutdown()
{
	Scripts.EndPlay();
	if (Rhi)
	{
		SceneRenderer.Shutdown();
		Resources.Shutdown();
		Rhi->Shutdown();
		Rhi.reset();
	}
}

void FRuntimeApplication::BuildPlaceholderScene()
{
	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetTransform(Sun).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
	FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
	SunLight.Intensity = 3.0f;

	const FMeshHandle CubeMesh = Resources.CreateMesh(FPrimitiveShapes::MakeCube(FUnits::MetersToUnits), L"Cube");

	FMaterial GroundMaterial;
	GroundMaterial.Name                    = "Ground";
	GroundMaterial.Constants.BaseColorFactor = FVector4(0.55f, 0.6f, 0.65f, 1.0f);
	GroundMaterial.Constants.Roughness       = 0.8f;

	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
	FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
	GroundMesh.Mesh     = CubeMesh;
	GroundMesh.Material = Resources.CreateMaterial(GroundMaterial);

	Scene.UpdateTransforms();
}

void FRuntimeApplication::OnScreenshotRequested(const std::filesystem::path& Path)
{
	if (Rhi)
	{
		Rhi->RequestScreenshot(Path);
	}
}

#include "Core/Application.h"
#include "Core/Math/Math.h"
#include "Core/Paths.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <filesystem>
#include <format>
#include <memory>

E_DEFINE_LOG_CATEGORY(LogSandbox, Log)

// 엔진 기능 검증용 애플리케이션: ECS 씬 + 씬 렌더러
class FSandboxApp final : public FApplication
{
public:
	FSandboxApp()
		: FApplication(MakeDesc())
	{
	}

protected:
	bool OnInit() override
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

		BuildScene();

		Camera.SetPerspective(60.0f, static_cast<float>(RhiDesc.Width) / static_cast<float>(RhiDesc.Height), 0.1f, 1000.0f);
		Camera.SetPosition(FVector3(-7.0f, -5.0f, 3.5f));
		Camera.LookAt(FVector3(0.0f, 0.0f, 0.8f));

		E_LOG(LogSandbox, Display, "조작: ESC 종료, F1 VSync, F2 컬링 프러스텀 고정, 우클릭 + 마우스/WASD/QE 시점 이동, 휠 속도");
		return true;
	}

	void OnUpdate(float DeltaSeconds) override
	{
		const FInput& InputState = GetInput();

		if (InputState.IsKeyPressed(EKey::Escape))
		{
			RequestExit();
		}
		if (InputState.IsKeyPressed(EKey::F1))
		{
			Rhi->SetVSync(!Rhi->IsVSync());
			E_LOG(LogSandbox, Display, "VSync: {}", Rhi->IsVSync() ? "켜짐" : "꺼짐");
		}
		if (InputState.IsKeyPressed(EKey::F2))
		{
			SceneRenderer.SetFreezeCulling(!SceneRenderer.IsCullingFrozen());
			E_LOG(LogSandbox, Display, "컬링 프러스텀 고정: {}", SceneRenderer.IsCullingFrozen() ? "켜짐" : "꺼짐");
		}

		CameraController.Update(Camera, InputState, DeltaSeconds);

		// 애니메이션: 큐브 링 공전(부모 회전 → 자식이 따라 돎), 헬멧 자전
		OrbitYawDegrees = FMath::Fmod(OrbitYawDegrees + 20.0f * DeltaSeconds, 360.0f);
		Scene.GetTransform(OrbitRoot).Rotation = FQuat::FromEuler(0.0f, OrbitYawDegrees, 0.0f);
		if (Scene.GetRegistry().IsValid(HelmetRoot))
		{
			HelmetYawDegrees = FMath::Fmod(HelmetYawDegrees + 10.0f * DeltaSeconds, 360.0f);
			Scene.GetTransform(HelmetRoot).Rotation = FQuat::FromEuler(0.0f, HelmetYawDegrees, 0.0f);
		}

		Scene.UpdateTransforms();
		UpdateTitleStats(DeltaSeconds);
	}

	void OnRender() override
	{
		const float ClearColor[4] = { 0.12f, 0.2f, 0.36f, 1.0f }; // 선형 공간 값 (sRGB 백버퍼가 인코딩)
		Rhi->BeginFrame(ClearColor);
		SceneRenderer.Render(Scene, Camera, Rhi->GetBackBufferOutput());
		Rhi->EndFrame();
	}

	void OnResize(uint32 Width, uint32 Height) override
	{
		if (Rhi)
		{
			Rhi->Resize(Width, Height);
			Camera.SetAspectRatio(static_cast<float>(Width) / static_cast<float>(Height));
		}
	}

	void OnScreenshotRequested(const std::filesystem::path& Path) override
	{
		if (Rhi)
		{
			Rhi->RequestScreenshot(Path);
		}
	}

	void OnShutdown() override
	{
		if (Rhi)
		{
			SceneRenderer.Shutdown();
			Resources.Shutdown();
			Rhi->Shutdown();
			Rhi.reset();
		}
	}

private:
	static FApplicationDesc MakeDesc()
	{
		FApplicationDesc Desc;
		Desc.Window.Title  = L"ProjectE Sandbox";
		Desc.Window.Width  = 1280;
		Desc.Window.Height = 720;
		return Desc;
	}

	void BuildScene()
	{
		if (!FPaths::HasProject())
		{
			E_LOG(LogSandbox, Warning, "열린 프로젝트가 없어 에셋을 로드할 수 없습니다 (--project <경로>)");
		}
		const std::filesystem::path AssetDir = FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : std::filesystem::path();

		// 태양광: 앞-왼쪽-위에서 비스듬히
		const FEntity Sun = Scene.CreateEntity("Sun");
		Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-50.0f, 30.0f, 0.0f);
		FDirectionalLightComponent& SunLight = Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
		SunLight.Color     = FVector3(1.0f, 0.96f, 0.9f);
		SunLight.Intensity = 3.0f;

		// 공용 리소스
		const FMeshHandle    CubeMesh       = Resources.CreateMesh(FPrimitiveShapes::MakeCube(1.0f), L"Cube");
		const FTextureHandle CheckerTexture = Resources.LoadTexture(AssetDir / L"UVChecker.png", true);

		FMaterial CheckerMaterial;
		CheckerMaterial.Name                       = "Checker";
		CheckerMaterial.Textures[MaterialSlot_BaseColor] = CheckerTexture;
		CheckerMaterial.Constants.Roughness              = 0.8f;
		const FMaterialHandle CheckerHandle = Resources.CreateMaterial(CheckerMaterial);

		// 바닥: 납작한 큐브
		const FEntity Ground = Scene.CreateEntity("Ground");
		Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -1.0f);
		Scene.GetTransform(Ground).Scale    = FVector3(20.0f, 20.0f, 0.2f);
		FStaticMeshComponent& GroundMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Ground);
		GroundMesh.Mesh     = CubeMesh;
		GroundMesh.Material = CheckerHandle;

		// 큐브 링: 회전하는 부모 아래 8개 자식 (계층 검증). 색상 틴트를 달리한 머티리얼
		OrbitRoot = Scene.CreateEntity("OrbitRoot");
		Scene.GetTransform(OrbitRoot).Position = FVector3(0.0f, 0.0f, 0.5f);
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const float Angle = FMath::DegreesToRadians(45.0f * static_cast<float>(Index));

			FMaterial TintMaterial;
			TintMaterial.Name                    = std::format("Tint{}", Index);
			TintMaterial.Textures[MaterialSlot_BaseColor] = CheckerTexture;
			TintMaterial.Constants.BaseColorFactor        = FVector4(0.5f + 0.5f * FMath::Cos(Angle), 0.5f + 0.5f * FMath::Sin(Angle), 0.7f, 1.0f);
			TintMaterial.Constants.Roughness              = 0.35f;
			TintMaterial.Constants.Metallic               = Index % 2 == 0 ? 1.0f : 0.0f;

			const FEntity Orbiter = Scene.CreateEntity(std::format("Orbiter{}", Index));
			Scene.SetParent(Orbiter, OrbitRoot);
			Scene.GetTransform(Orbiter).Position = FVector3(4.0f * FMath::Cos(Angle), 4.0f * FMath::Sin(Angle), 0.0f);
			Scene.GetTransform(Orbiter).Rotation = FQuat::FromEuler(0.0f, 45.0f * static_cast<float>(Index), 0.0f);
			Scene.GetTransform(Orbiter).Scale    = FVector3(0.6f);
			FStaticMeshComponent& OrbiterMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Orbiter);
			OrbiterMesh.Mesh     = CubeMesh;
			OrbiterMesh.Material = Resources.CreateMaterial(TintMaterial);
		}

		// 멀리 떨어진 큐브들: 컬링 통계 확인용
		for (int32 Index = 0; Index < 40; ++Index)
		{
			const FEntity Far = Scene.CreateEntity(std::format("FarCube{}", Index));
			const float   Angle = FMath::DegreesToRadians(9.0f * static_cast<float>(Index));
			Scene.GetTransform(Far).Position = FVector3(30.0f * FMath::Cos(Angle), 30.0f * FMath::Sin(Angle), 1.0f + (Index % 5));
			FStaticMeshComponent& FarMesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Far);
			FarMesh.Mesh     = CubeMesh;
			FarMesh.Material = CheckerHandle;
		}

		// glTF 모델
		HelmetRoot = FModelLoader::LoadIntoScene(AssetDir / L"DamagedHelmet.glb", Scene, Resources);
		if (Scene.GetRegistry().IsValid(HelmetRoot))
		{
			Scene.GetTransform(HelmetRoot).Position = FVector3(0.0f, 0.0f, 1.2f);
		}
		else
		{
			E_LOG(LogSandbox, Warning, "DamagedHelmet.glb 로드 실패 — 큐브 씬만 표시합니다");
		}

		Scene.UpdateTransforms();
		E_LOG(LogSandbox, Display, "씬 구성 완료: 엔티티 {}개, 메시 {}개, 머티리얼 {}개, 텍스처 {}개",
		      Scene.GetRegistry().GetAliveCount(), Resources.GetMeshCount(), Resources.GetMaterialCount(), Resources.GetTextureCount());
	}

	// 0.5초마다 창 제목에 FPS / 프레임 시간 / 컬링 통계 표시
	void UpdateTitleStats(float DeltaSeconds)
	{
		StatsAccumulatedSeconds += DeltaSeconds;
		++StatsFrameCount;

		if (StatsAccumulatedSeconds >= 0.5f)
		{
			const float              Fps         = static_cast<float>(StatsFrameCount) / StatsAccumulatedSeconds;
			const float              FrameTimeMs = 1000.0f / Fps;
			const FSceneRenderStats& Stats       = SceneRenderer.GetStats();

			GetWindow().SetTitle(std::format(L"ProjectE Sandbox | {:.1f} FPS | {:.2f} ms | 메시 {}/{} 표시 | 드로우 {} | VSync {}{}",
			                                 Fps, FrameTimeMs, Stats.VisibleMeshes, Stats.TotalMeshes, Stats.DrawCalls,
			                                 Rhi->IsVSync() ? L"On" : L"Off", SceneRenderer.IsCullingFrozen() ? L" | 컬링 고정" : L""));

			StatsAccumulatedSeconds = 0.0f;
			StatsFrameCount         = 0;
		}
	}

	std::unique_ptr<FD3D12RHI> Rhi;
	FResourceManager           Resources;
	FSceneRenderer             SceneRenderer;
	FScene                     Scene;

	FCamera              Camera;
	FFlyCameraController CameraController;

	FEntity OrbitRoot;
	FEntity HelmetRoot;
	float   OrbitYawDegrees  = 0.0f;
	float   HelmetYawDegrees = 0.0f;

	float  StatsAccumulatedSeconds = 0.0f;
	uint32 StatsFrameCount         = 0;
};

int main()
{
	FSandboxApp App;
	return App.Run();
}

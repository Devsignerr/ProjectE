#include "Core/Testing/TestFramework.h"
#include "Renderer/Camera.h"
#include "Renderer/SceneCamera.h"
#include "Scene/CameraProjection.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

// FCameraProjection (Scene/CameraProjection.h): 렌더러 FCamera와 같은 행렬 + 뷰포트 변환 + 광선
namespace
{
	FMatrix4x4 MakeRendererViewProjection(FScene& Scene, FEntity Entity, float Aspect)
	{
		FCamera Camera;
		E_EXPECT_TRUE(FSceneCamera::ApplyToCamera(Scene, Entity, Aspect, Camera));
		return Camera.GetUnjitteredViewProjectionMatrix();
	}
} // namespace

E_TEST(CameraProjection_MatchesRendererCamera)
{
	FScene        Scene;
	const FEntity Entity                    = Scene.CreateEntity("Camera");
	FCameraComponent& Camera                = Scene.GetRegistry().Emplace<FCameraComponent>(Entity);
	Camera.FovYDegrees                      = 50.0f;
	Camera.NearZ                            = 5.0f;
	Camera.FarZ                             = 20000.0f;
	Scene.GetTransform(Entity).Position     = FVector3(-300.0f, 120.0f, 450.0f);
	Scene.GetTransform(Entity).Rotation     = FQuat::FromEuler(-35.0f, 45.0f, 0.0f);
	Scene.UpdateTransforms();

	for (const bool bOrthographic : { false, true })
	{
		Scene.GetRegistry().Get<FCameraComponent>(Entity).bOrthographic = bOrthographic;
		Scene.GetRegistry().Get<FCameraComponent>(Entity).OrthoHeight   = 900.0f;
		FMatrix4x4 Mine;
		E_EXPECT_TRUE(FCameraProjection::ComputeViewProjection(Scene, Entity, 16.0f / 9.0f, Mine));
		E_EXPECT_TRUE(Mine.Equals(MakeRendererViewProjection(Scene, Entity, 16.0f / 9.0f), 1.0e-5f));
	}
}

E_TEST(CameraProjection_ActiveCameraUsesPriority)
{
	FScene        Scene;
	const FEntity A = Scene.CreateEntity("A");
	const FEntity B = Scene.CreateEntity("B");
	const FEntity C = Scene.CreateEntity("C");
	Scene.GetRegistry().Emplace<FCameraComponent>(A);
	Scene.GetRegistry().Emplace<FCameraComponent>(B).Priority = 5;
	FCameraComponent& NotPrimary = Scene.GetRegistry().Emplace<FCameraComponent>(C);
	NotPrimary.Priority          = 100;
	NotPrimary.bPrimary          = false;
	E_EXPECT_TRUE(FCameraProjection::FindActiveCamera(Scene) == B);
	E_EXPECT_TRUE(FSceneCamera::FindPrimary(Scene) == B); // 렌더러와 같은 규칙
}

E_TEST(CameraProjection_WorldToViewportAndRayRoundTrip)
{
	FCameraComponent Camera;
	Camera.FovYDegrees     = 90.0f;
	const FVector2   Size(800.0f, 600.0f);
	const FMatrix4x4 ViewProjection = FCameraProjection::MakeViewProjection(FVector3::ZeroVector, FQuat::Identity, Camera, Size.X / Size.Y);

	FVector2 Pixel;
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(1000.0f, 0.0f, 0.0f), Size, Pixel));
	E_EXPECT_EQUALS(Pixel, FVector2(400.0f, 300.0f), 1.0e-3f);
	// +Y 오른쪽, +Z 위 → 화면 오른쪽 위 (FOV 90, 4:3)
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(1000.0f, 1000.0f, 500.0f), Size, Pixel));
	E_EXPECT_EQUALS(Pixel, FVector2(700.0f, 150.0f), 1.0e-2f);
	E_EXPECT_FALSE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(-1000.0f, 0.0f, 0.0f), Size, Pixel)); // 뒤
	E_EXPECT_FALSE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(5.0f, 0.0f, 0.0f), Size, Pixel));     // 근평면 앞
	E_EXPECT_FALSE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(1000.0f, 0.0f, 1100.0f), Size, Pixel)); // 위로 벗어남

	// 광선 → 그 위의 점을 다시 투영하면 같은 픽셀
	FVector3 Origin;
	FVector3 Direction;
	E_EXPECT_TRUE(FCameraProjection::ViewportToWorldRay(ViewProjection, FVector2(123.0f, 456.0f), Size, Origin, Direction));
	E_EXPECT_NEAR(Direction.Length(), 1.0f, 1.0e-4f);
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(ViewProjection, Origin + Direction * 2000.0f, Size, Pixel));
	E_EXPECT_EQUALS(Pixel, FVector2(123.0f, 456.0f), 5.0e-2f);

	// 직교: 광선은 모두 Forward와 평행
	Camera.bOrthographic                  = true;
	Camera.OrthoHeight                    = 600.0f;
	const FQuat      Down                 = FQuat::FromEuler(-90.0f, 0.0f, 0.0f);
	const FMatrix4x4 Ortho = FCameraProjection::MakeViewProjection(FVector3(0.0f, 0.0f, 1000.0f), Down, Camera, Size.X / Size.Y);
	E_EXPECT_TRUE(FCameraProjection::ViewportToWorldRay(Ortho, FVector2(10.0f, 20.0f), Size, Origin, Direction));
	E_EXPECT_EQUALS(Direction, FVector3(0.0f, 0.0f, -1.0f), 1.0e-4f);
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(Ortho, FVector3(Origin.X, Origin.Y, 0.0f), Size, Pixel));
	E_EXPECT_EQUALS(Pixel, FVector2(10.0f, 20.0f), 1.0e-2f);
}

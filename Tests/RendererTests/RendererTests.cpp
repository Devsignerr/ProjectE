#include "Core/Testing/TestFramework.h"
#include "Renderer/Camera.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/SceneCamera.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(Cube_Topology)
{
	const FMeshData Cube = FPrimitiveShapes::MakeCube(2.0f);
	E_EXPECT_EQ(Cube.Vertices.size(), static_cast<size_t>(24));
	E_EXPECT_EQ(Cube.Indices.size(), static_cast<size_t>(36));

	for (const FVertex& Vertex : Cube.Vertices)
	{
		// 한 변 2 → 모든 꼭짓점 좌표는 ±1
		E_EXPECT_NEAR(FMath::Abs(Vertex.Position.X), 1.0f, Tol);
		E_EXPECT_NEAR(FMath::Abs(Vertex.Position.Y), 1.0f, Tol);
		E_EXPECT_NEAR(FMath::Abs(Vertex.Position.Z), 1.0f, Tol);
		E_EXPECT_TRUE(Vertex.Normal.IsNormalized());
		// 법선은 바깥을 향한다
		E_EXPECT_TRUE(FVector3::Dot(Vertex.Position, Vertex.Normal) > 0.0f);
		// UV는 [0, 1]
		E_EXPECT_TRUE(Vertex.UV.X >= 0.0f && Vertex.UV.X <= 1.0f && Vertex.UV.Y >= 0.0f && Vertex.UV.Y <= 1.0f);
	}
}

E_TEST(Cube_WindingIsClockwiseFromOutside)
{
	const FMeshData Cube = FPrimitiveShapes::MakeCube(1.0f);

	for (size_t Index = 0; Index + 2 < Cube.Indices.size(); Index += 3)
	{
		const FVertex& V0 = Cube.Vertices[Cube.Indices[Index + 0]];
		const FVertex& V1 = Cube.Vertices[Cube.Indices[Index + 1]];
		const FVertex& V2 = Cube.Vertices[Cube.Indices[Index + 2]];

		// 왼손 좌표계에서 바깥에서 본 시계 방향 삼각형은 Cross(E1, E2)가 바깥 법선과 같은 방향
		const FVector3 GeometricNormal = FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position);
		E_EXPECT_TRUE(FVector3::Dot(GeometricNormal, V0.Normal) > 0.0f);

		// 삼각형의 세 정점은 같은 면(법선 동일)
		E_EXPECT_EQUALS(V0.Normal, V1.Normal, Tol);
		E_EXPECT_EQUALS(V0.Normal, V2.Normal, Tol);
	}
}

E_TEST(Camera_LookAtAndView)
{
	FCamera Camera;
	Camera.SetPosition(FVector3(-10.0f, 0.0f, 0.0f));
	Camera.LookAt(FVector3::ZeroVector);

	E_EXPECT_EQUALS(Camera.GetForwardVector(), FVector3::ForwardVector, Tol);
	E_EXPECT_EQUALS(Camera.GetUpVector(), FVector3::UpVector, Tol);

	// 원점은 카메라 앞 10 (뷰 공간 +Z)
	const FMatrix4x4 View = Camera.GetViewMatrix();
	E_EXPECT_EQUALS(View.TransformPosition(FVector3::ZeroVector), FVector3(0.0f, 0.0f, 10.0f), Tol);

	// 위를 올려다보면 Pitch 양수: 원점보다 위에 있는 점이 화면 중앙으로 온다
	Camera.LookAt(FVector3(0.0f, 0.0f, 10.0f));
	const FVector3 Centered = Camera.GetViewMatrix().TransformPosition(FVector3(0.0f, 0.0f, 10.0f));
	E_EXPECT_NEAR(Centered.X, 0.0f, Tol);
	E_EXPECT_NEAR(Centered.Y, 0.0f, Tol);
	E_EXPECT_TRUE(Centered.Z > 0.0f);
}

E_TEST(Camera_ProjectionClipRange)
{
	FCamera Camera;
	Camera.SetPerspective(90.0f, 1.0f, 1.0f, 100.0f);
	Camera.SetPosition(FVector3::ZeroVector);
	Camera.SetRotation(FQuat::Identity);

	const FMatrix4x4 ViewProj = Camera.GetViewProjectionMatrix();
	const FVector4   NearClip = ViewProj.TransformVector4(FVector4(1.0f, 0.0f, 0.0f, 1.0f));
	const FVector4   FarClip  = ViewProj.TransformVector4(FVector4(100.0f, 0.0f, 0.0f, 1.0f));
	E_EXPECT_NEAR(NearClip.Z / NearClip.W, 0.0f, Tol);
	E_EXPECT_NEAR(FarClip.Z / FarClip.W, 1.0f, Tol);
}

E_TEST(PrimitiveShapes_CapsuleWindingAndShape)
{
	// 반지름 30, 원기둥 절반 60 → 높이 180 (캡슐 콜라이더와 같은 정의)
	const FMeshData Capsule = FPrimitiveShapes::MakeCapsule(30.0f, 60.0f, 16, 6);
	E_EXPECT_TRUE(!Capsule.Indices.empty() && Capsule.Indices.size() % 3 == 0);
	bool bAllClockwise = true;
	for (size_t Index = 0; Index < Capsule.Indices.size(); Index += 3)
	{
		const FVertex& V0     = Capsule.Vertices[Capsule.Indices[Index]];
		const FVertex& V1     = Capsule.Vertices[Capsule.Indices[Index + 1]];
		const FVertex& V2     = Capsule.Vertices[Capsule.Indices[Index + 2]];
		const FVector3 Normal = V0.Normal + V1.Normal + V2.Normal;
		bAllClockwise         = bAllClockwise && FVector3::Dot(FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position), Normal) > 0.0f;
	}
	E_EXPECT_TRUE(bAllClockwise);
	float MinZ = 1.0e9f;
	float MaxZ = -1.0e9f;
	for (const FVertex& Vertex : Capsule.Vertices)
	{
		MinZ = std::min(MinZ, Vertex.Position.Z);
		MaxZ = std::max(MaxZ, Vertex.Position.Z);
		// 축에서의 거리 = 반지름 (반구 부분은 반구 중심 기준)
		const float    CenterZ = std::clamp(Vertex.Position.Z, -60.0f, 60.0f);
		const FVector3 FromAxis(Vertex.Position.X, Vertex.Position.Y, Vertex.Position.Z - CenterZ);
		E_EXPECT_NEAR(FromAxis.Length(), 30.0f, 1.0e-3f);
	}
	E_EXPECT_NEAR(MinZ, -90.0f, 1.0e-3f);
	E_EXPECT_NEAR(MaxZ, 90.0f, 1.0e-3f);
}

E_TEST(SceneCamera_PriorityWins)
{
	FScene        Scene;
	const FEntity Level  = Scene.CreateEntity("LevelCamera");
	const FEntity Player = Scene.CreateEntity("PlayerCamera");
	const FEntity Off    = Scene.CreateEntity("OffCamera");
	Scene.GetRegistry().Emplace<FCameraComponent>(Level);
	Scene.GetRegistry().Emplace<FCameraComponent>(Player);
	Scene.GetRegistry().Emplace<FCameraComponent>(Off); // (Emplace는 저장소를 옮길 수 있어 참조는 다 넣은 뒤에 얻는다)
	FCameraComponent& PlayerCamera = Scene.GetRegistry().Get<FCameraComponent>(Player);
	FCameraComponent& OffCamera    = Scene.GetRegistry().Get<FCameraComponent>(Off);
	OffCamera.bPrimary = false;
	OffCamera.Priority = 100; // 주 카메라가 아니면 우선순위와 무관
	E_EXPECT_TRUE(FSceneCamera::FindPrimary(Scene) == Level); // 같은 우선순위면 먼저 찾은 것
	PlayerCamera.Priority = 10;
	E_EXPECT_TRUE(FSceneCamera::FindPrimary(Scene) == Player);
	PlayerCamera.bPrimary = false;
	E_EXPECT_TRUE(FSceneCamera::FindPrimary(Scene) == Level);
}

E_TEST(PrimitiveShapes_SphereWindingAndRadius)
{
	const FMeshData Sphere = FPrimitiveShapes::MakeSphere(50.0f, 16, 8);
	E_EXPECT_TRUE(!Sphere.Indices.empty() && Sphere.Indices.size() % 3 == 0);
	bool bAllClockwise = true;
	for (size_t Index = 0; Index < Sphere.Indices.size(); Index += 3)
	{
		const FVertex& V0     = Sphere.Vertices[Sphere.Indices[Index]];
		const FVertex& V1     = Sphere.Vertices[Sphere.Indices[Index + 1]];
		const FVertex& V2     = Sphere.Vertices[Sphere.Indices[Index + 2]];
		const FVector3 Normal = V0.Normal + V1.Normal + V2.Normal;
		bAllClockwise         = bAllClockwise && FVector3::Dot(FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position), Normal) > 0.0f;
	}
	E_EXPECT_TRUE(bAllClockwise);
	for (const FVertex& Vertex : Sphere.Vertices)
	{
		E_EXPECT_NEAR(Vertex.Position.Length(), 50.0f, 1.0e-3f);
	}
}

#include "Core/Testing/TestFramework.h"
#include "Renderer/Camera.h"
#include "Renderer/PrimitiveShapes.h"

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

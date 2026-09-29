#include "Core/Math/Math.h"
#include "Core/Testing/TestFramework.h"

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

// ---------------------------------------------------------------- FVector3

E_TEST(Vector3_BasicArithmetic)
{
	const FVector3 A(1.0f, 2.0f, 3.0f);
	const FVector3 B(4.0f, 5.0f, 6.0f);

	E_EXPECT_EQUALS(A + B, FVector3(5.0f, 7.0f, 9.0f), Tol);
	E_EXPECT_EQUALS(B - A, FVector3(3.0f, 3.0f, 3.0f), Tol);
	E_EXPECT_EQUALS(A * 2.0f, FVector3(2.0f, 4.0f, 6.0f), Tol);
	E_EXPECT_EQUALS(2.0f * A, FVector3(2.0f, 4.0f, 6.0f), Tol);
	E_EXPECT_EQUALS(B / 2.0f, FVector3(2.0f, 2.5f, 3.0f), Tol);
	E_EXPECT_EQUALS(-A, FVector3(-1.0f, -2.0f, -3.0f), Tol);
}

E_TEST(Vector3_DotAndCross)
{
	E_EXPECT_NEAR(FVector3::Dot(FVector3(1.0f, 2.0f, 3.0f), FVector3(4.0f, 5.0f, 6.0f)), 32.0f, Tol);
	E_EXPECT_NEAR(FVector3::Dot(FVector3::ForwardVector, FVector3::RightVector), 0.0f, Tol);

	// 좌표계 규약: Forward × Right == Up
	E_EXPECT_EQUALS(FVector3::Cross(FVector3::ForwardVector, FVector3::RightVector), FVector3::UpVector, Tol);
	E_EXPECT_EQUALS(FVector3::Cross(FVector3::RightVector, FVector3::UpVector), FVector3::ForwardVector, Tol);
	E_EXPECT_EQUALS(FVector3::Cross(FVector3::UpVector, FVector3::ForwardVector), FVector3::RightVector, Tol);
}

E_TEST(Vector3_LengthAndNormalize)
{
	const FVector3 V(3.0f, 4.0f, 0.0f);
	E_EXPECT_NEAR(V.Length(), 5.0f, Tol);
	E_EXPECT_NEAR(V.LengthSquared(), 25.0f, Tol);
	E_EXPECT_EQUALS(V.GetNormalized(), FVector3(0.6f, 0.8f, 0.0f), Tol);
	E_EXPECT_TRUE(V.GetNormalized().IsNormalized());

	// 영벡터 정규화는 영벡터
	E_EXPECT_EQUALS(FVector3::ZeroVector.GetNormalized(), FVector3::ZeroVector, Tol);
	FVector3 Zero;
	E_EXPECT_FALSE(Zero.Normalize());

	E_EXPECT_NEAR(FVector3::Distance(FVector3(1.0f, 1.0f, 1.0f), FVector3(1.0f, 1.0f, 4.0f)), 3.0f, Tol);
	E_EXPECT_EQUALS(FVector3::Lerp(FVector3::ZeroVector, FVector3(10.0f, 20.0f, 30.0f), 0.5f), FVector3(5.0f, 10.0f, 15.0f), Tol);
}

// ---------------------------------------------------------------- FQuat

E_TEST(Quat_IdentityAndAxisAngle)
{
	E_EXPECT_EQUALS(FQuat::Identity.RotateVector(FVector3(1.0f, 2.0f, 3.0f)), FVector3(1.0f, 2.0f, 3.0f), Tol);

	// +Yaw 90°: Forward → Right
	const FQuat Yaw90 = FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi);
	E_EXPECT_TRUE(Yaw90.IsNormalized());
	E_EXPECT_EQUALS(Yaw90.RotateVector(FVector3::ForwardVector), FVector3::RightVector, Tol);
	E_EXPECT_EQUALS(Yaw90.GetForwardVector(), FVector3::RightVector, Tol);
	E_EXPECT_EQUALS(Yaw90.GetRightVector(), -FVector3::ForwardVector, Tol);
	E_EXPECT_EQUALS(Yaw90.GetUpVector(), FVector3::UpVector, Tol);

	// 역회전
	E_EXPECT_EQUALS(Yaw90.UnrotateVector(FVector3::RightVector), FVector3::ForwardVector, Tol);
	E_EXPECT_EQUALS(Yaw90.Inverse().RotateVector(FVector3::RightVector), FVector3::ForwardVector, Tol);
	E_EXPECT_EQUALS(Yaw90 * Yaw90.Inverse(), FQuat::Identity, Tol);
}

E_TEST(Quat_EulerConvention)
{
	// +Pitch: 기수 위로 (Forward → Up)
	E_EXPECT_EQUALS(FQuat::FromEuler(90.0f, 0.0f, 0.0f).GetForwardVector(), FVector3::UpVector, Tol);
	// +Yaw: 오른쪽으로 (Forward → Right)
	E_EXPECT_EQUALS(FQuat::FromEuler(0.0f, 90.0f, 0.0f).GetForwardVector(), FVector3::RightVector, Tol);
	// +Roll: 오른쪽 날개 아래로 (Right → -Up)
	E_EXPECT_EQUALS(FQuat::FromEuler(0.0f, 0.0f, 90.0f).GetRightVector(), -FVector3::UpVector, Tol);

	// Roll → Pitch → Yaw 순서: Yaw 90 후 Pitch 90 → Forward가 Up으로 (Pitch 축이 Yaw에 의해 회전됨)
	const FQuat Combined = FQuat::FromEuler(90.0f, 90.0f, 0.0f);
	E_EXPECT_EQUALS(Combined.GetForwardVector(), FVector3::UpVector, Tol);
	E_EXPECT_EQUALS(Combined.GetRightVector(), -FVector3::ForwardVector, Tol);
}

E_TEST(Quat_Composition)
{
	const FQuat A = FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi);
	const FQuat B = FQuat::FromAxisAngle(FVector3::RightVector, FMath::HalfPi);
	const FVector3 V(1.0f, 2.0f, 3.0f);

	// (A * B): B 먼저, A 나중
	E_EXPECT_EQUALS((A * B).RotateVector(V), A.RotateVector(B.RotateVector(V)), Tol);
	E_EXPECT_TRUE((A * B).IsNormalized());
}

E_TEST(Quat_Slerp)
{
	const FQuat A = FQuat::Identity;
	const FQuat B = FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi);

	E_EXPECT_EQUALS(FQuat::Slerp(A, B, 0.0f), A, Tol);
	E_EXPECT_EQUALS(FQuat::Slerp(A, B, 1.0f), B, Tol);

	// 중간값은 45° 회전
	const FQuat Mid = FQuat::Slerp(A, B, 0.5f);
	E_EXPECT_EQUALS(Mid, FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi * 0.5f), Tol);
	E_EXPECT_TRUE(Mid.IsNormalized());

	// -B는 같은 회전이므로 최단 경로로 동일한 결과
	const FQuat NegB(-B.X, -B.Y, -B.Z, -B.W);
	E_EXPECT_EQUALS(FQuat::Slerp(A, NegB, 0.5f), Mid, Tol);
}

// ---------------------------------------------------------------- FMatrix4x4

E_TEST(Matrix_IdentityAndMultiply)
{
	const FMatrix4x4 I;
	E_EXPECT_EQUALS(I, FMatrix4x4::Identity, Tol);

	const FMatrix4x4 T = FMatrix4x4::MakeTranslation(FVector3(1.0f, 2.0f, 3.0f));
	E_EXPECT_EQUALS(I * T, T, Tol);
	E_EXPECT_EQUALS(T * I, T, Tol);

	// 이동 두 번 = 합산
	const FMatrix4x4 T2 = T * T;
	E_EXPECT_EQUALS(T2.GetOrigin(), FVector3(2.0f, 4.0f, 6.0f), Tol);
}

E_TEST(Matrix_TransformPositionAndVector)
{
	const FMatrix4x4 T = FMatrix4x4::MakeTranslation(FVector3(10.0f, 0.0f, 0.0f));
	const FVector3   P(1.0f, 2.0f, 3.0f);

	// 위치는 이동되고, 방향은 이동되지 않는다
	E_EXPECT_EQUALS(T.TransformPosition(P), FVector3(11.0f, 2.0f, 3.0f), Tol);
	E_EXPECT_EQUALS(T.TransformVector(P), P, Tol);

	const FMatrix4x4 S = FMatrix4x4::MakeScale(FVector3(2.0f, 3.0f, 4.0f));
	E_EXPECT_EQUALS(S.TransformPosition(P), FVector3(2.0f, 6.0f, 12.0f), Tol);

	const FVector4 H = T.TransformVector4(FVector4(P, 1.0f));
	E_EXPECT_EQUALS(H, FVector4(11.0f, 2.0f, 3.0f, 1.0f), Tol);
}

E_TEST(Matrix_RotationMatchesQuat)
{
	const FQuat      Q = FQuat::FromEuler(30.0f, 45.0f, 60.0f);
	const FMatrix4x4 R = FMatrix4x4::MakeRotation(Q);
	const FVector3   V(1.0f, 2.0f, 3.0f);

	E_EXPECT_EQUALS(R.TransformVector(V), Q.RotateVector(V), Tol);

	// 행벡터 규약: v * (Ra * Rb) == (v * Ra) * Rb  ⇔  FromQuat(A*B) == FromQuat(B) * FromQuat(A)
	const FQuat A = FQuat::FromAxisAngle(FVector3::UpVector, 0.7f);
	const FQuat B = FQuat::FromAxisAngle(FVector3::RightVector, -1.1f);
	E_EXPECT_EQUALS(FMatrix4x4::MakeRotation(A * B), FMatrix4x4::MakeRotation(B) * FMatrix4x4::MakeRotation(A), Tol);
}

E_TEST(Matrix_MakeTransformOrder)
{
	const FVector3 Translation(5.0f, -2.0f, 1.0f);
	const FQuat    Rotation = FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi);
	const FVector3 Scale(2.0f, 2.0f, 2.0f);

	const FMatrix4x4 W = FMatrix4x4::MakeTransform(Translation, Rotation, Scale);

	// Scale → Rotation → Translation 순서: (1,0,0) → (2,0,0) → (0,2,0) → (5,0,1)
	E_EXPECT_EQUALS(W.TransformPosition(FVector3::ForwardVector), FVector3(5.0f, 0.0f, 1.0f), Tol);
	E_EXPECT_EQUALS(W.GetOrigin(), Translation, Tol);
}

E_TEST(Matrix_TransposeDeterminantInverse)
{
	const FMatrix4x4 W = FMatrix4x4::MakeTransform(FVector3(1.0f, 2.0f, 3.0f),
	                                                FQuat::FromEuler(10.0f, 20.0f, 30.0f),
	                                                FVector3(2.0f, 3.0f, 4.0f));

	E_EXPECT_EQUALS(W.GetTransposed().GetTransposed(), W, Tol);
	E_EXPECT_NEAR(W.Determinant(), 24.0f, 1.0e-3f); // 스케일 곱 (회전/이동은 행렬식 1)

	FMatrix4x4 Inv;
	E_EXPECT_TRUE(W.TryGetInverse(Inv));
	E_EXPECT_EQUALS(W * Inv, FMatrix4x4::Identity, 1.0e-3f);
	E_EXPECT_EQUALS(Inv * W, FMatrix4x4::Identity, 1.0e-3f);

	// 특이 행렬
	const FMatrix4x4 Singular = FMatrix4x4::MakeScale(FVector3(1.0f, 0.0f, 1.0f));
	FMatrix4x4       Unused;
	E_EXPECT_FALSE(Singular.TryGetInverse(Unused));
	E_EXPECT_EQUALS(Singular.GetInverse(), FMatrix4x4::Identity, Tol);
}

E_TEST(Matrix_LookAt)
{
	// 카메라가 -X에서 원점을 바라봄 (앞 = +X)
	const FMatrix4x4 View = FMatrix4x4::MakeLookAt(FVector3(-10.0f, 0.0f, 0.0f), FVector3::ZeroVector);

	// 원점은 카메라 앞 10 (뷰 공간 +Z)
	E_EXPECT_EQUALS(View.TransformPosition(FVector3::ZeroVector), FVector3(0.0f, 0.0f, 10.0f), Tol);
	// 월드 위(+Z)는 뷰 공간 위(+Y)
	E_EXPECT_EQUALS(View.TransformVector(FVector3::UpVector), FVector3(0.0f, 1.0f, 0.0f), Tol);
	// 월드 오른쪽(+Y)은 뷰 공간 오른쪽(+X)
	E_EXPECT_EQUALS(View.TransformVector(FVector3::RightVector), FVector3(1.0f, 0.0f, 0.0f), Tol);
	// 뷰 행렬은 강체 변환: 역행렬은 카메라 월드 행렬
	E_EXPECT_EQUALS(View.GetInverse().GetOrigin(), FVector3(-10.0f, 0.0f, 0.0f), Tol);
}

E_TEST(Matrix_Perspective)
{
	const float      Near = 1.0f;
	const float      Far  = 100.0f;
	const FMatrix4x4 Proj = FMatrix4x4::MakePerspectiveFov(FMath::DegreesToRadians(90.0f), 1.0f, Near, Far);

	// 근평면 → 깊이 0, 원평면 → 깊이 1 (원근 나눗셈 후)
	const FVector4 NearClip = Proj.TransformVector4(FVector4(0.0f, 0.0f, Near, 1.0f));
	const FVector4 FarClip  = Proj.TransformVector4(FVector4(0.0f, 0.0f, Far, 1.0f));
	E_EXPECT_NEAR(NearClip.Z / NearClip.W, 0.0f, Tol);
	E_EXPECT_NEAR(FarClip.Z / FarClip.W, 1.0f, Tol);
	E_EXPECT_NEAR(NearClip.W, Near, Tol);

	// FOV 90°: 거리 z에서 x = z 인 점은 NDC 가장자리 (x/w = 1)
	const FVector4 Edge = Proj.TransformVector4(FVector4(5.0f, 0.0f, 5.0f, 1.0f));
	E_EXPECT_NEAR(Edge.X / Edge.W, 1.0f, Tol);
}

E_TEST(Matrix_Orthographic)
{
	const FMatrix4x4 Proj = FMatrix4x4::MakeOrthographic(20.0f, 10.0f, 0.0f, 50.0f);

	E_EXPECT_EQUALS(Proj.TransformPosition(FVector3(10.0f, 5.0f, 0.0f)), FVector3(1.0f, 1.0f, 0.0f), Tol);
	E_EXPECT_EQUALS(Proj.TransformPosition(FVector3(-10.0f, -5.0f, 50.0f)), FVector3(-1.0f, -1.0f, 1.0f), Tol);
}

// ---------------------------------------------------------------- FQuat ↔ FMatrix4x4

E_TEST(Quat_FromRotationMatrixRoundtrip)
{
	const FQuat Samples[] = {
		FQuat::Identity,
		FQuat::FromAxisAngle(FVector3::UpVector, FMath::HalfPi),
		FQuat::FromAxisAngle(FVector3::UpVector, FMath::Pi),          // 대각합 음수 분기
		FQuat::FromAxisAngle(FVector3::ForwardVector, FMath::Pi * 0.9f),
		FQuat::FromAxisAngle(FVector3::RightVector, -FMath::Pi * 0.95f),
		FQuat::FromEuler(30.0f, 120.0f, -80.0f),
		FQuat::FromEuler(-85.0f, 170.0f, 175.0f),
	};
	for (const FQuat& Q : Samples)
	{
		const FQuat Recovered = FQuat::FromRotationMatrix(FMatrix4x4::MakeRotation(Q).M);
		E_EXPECT_TRUE(Recovered.IsNormalized());
		E_EXPECT_EQUALS(Recovered, Q, 1.0e-3f);
	}
}

// ---------------------------------------------------------------- FBox

E_TEST(Box_BuildAndQuery)
{
	FBox Box;
	E_EXPECT_FALSE(Box.IsValid());

	Box.AddPoint(FVector3(1.0f, -2.0f, 3.0f));
	Box.AddPoint(FVector3(-1.0f, 2.0f, 5.0f));
	E_EXPECT_TRUE(Box.IsValid());
	E_EXPECT_EQUALS(Box.Min, FVector3(-1.0f, -2.0f, 3.0f), Tol);
	E_EXPECT_EQUALS(Box.Max, FVector3(1.0f, 2.0f, 5.0f), Tol);
	E_EXPECT_EQUALS(Box.GetCenter(), FVector3(0.0f, 0.0f, 4.0f), Tol);
	E_EXPECT_EQUALS(Box.GetExtent(), FVector3(1.0f, 2.0f, 1.0f), Tol);
	E_EXPECT_TRUE(Box.Contains(FVector3(0.0f, 0.0f, 4.0f)));
	E_EXPECT_FALSE(Box.Contains(FVector3(0.0f, 0.0f, 6.0f)));

	E_EXPECT_TRUE(Box.Intersects(FBox(FVector3(0.5f, 0.5f, 4.5f), FVector3(10.0f, 10.0f, 10.0f))));
	E_EXPECT_FALSE(Box.Intersects(FBox(FVector3(2.0f, 2.0f, 2.0f), FVector3(3.0f, 3.0f, 3.0f))));

	// 이동 변환은 그대로 옮기고, Yaw 90 회전은 X/Y 범위를 교환
	const FBox Moved = Box.TransformBy(FMatrix4x4::MakeTranslation(FVector3(10.0f, 0.0f, 0.0f)));
	E_EXPECT_EQUALS(Moved.GetCenter(), FVector3(10.0f, 0.0f, 4.0f), Tol);
	const FBox Rotated = Box.TransformBy(FMatrix4x4::MakeRotation(FQuat::FromEuler(0.0f, 90.0f, 0.0f)));
	E_EXPECT_EQUALS(Rotated.GetExtent(), FVector3(2.0f, 1.0f, 1.0f), Tol);

	// 비어 있는 상자는 변환해도 비어 있음
	E_EXPECT_FALSE(FBox().TransformBy(FMatrix4x4::Identity).IsValid());
}

// ---------------------------------------------------------------- FFrustum

E_TEST(Frustum_CullsBoxes)
{
	// 원점에서 +X를 바라보는 90° 카메라, 근평면 1, 원평면 100
	const FMatrix4x4 View = FMatrix4x4::MakeLookAt(FVector3::ZeroVector, FVector3::ForwardVector);
	const FMatrix4x4 Proj = FMatrix4x4::MakePerspectiveFov(FMath::DegreesToRadians(90.0f), 1.0f, 1.0f, 100.0f);
	const FFrustum   Frustum = FFrustum::FromViewProjection(View * Proj);

	const auto UnitBoxAt = [](const FVector3& Center) { return FBox(Center - FVector3(0.5f), Center + FVector3(0.5f)); };

	E_EXPECT_TRUE(Frustum.Contains(FVector3(10.0f, 0.0f, 0.0f)));
	E_EXPECT_FALSE(Frustum.Contains(FVector3(-10.0f, 0.0f, 0.0f)));

	E_EXPECT_TRUE(Frustum.Intersects(UnitBoxAt(FVector3(10.0f, 0.0f, 0.0f))));   // 정면
	E_EXPECT_FALSE(Frustum.Intersects(UnitBoxAt(FVector3(-10.0f, 0.0f, 0.0f))));  // 뒤
	E_EXPECT_FALSE(Frustum.Intersects(UnitBoxAt(FVector3(10.0f, 20.0f, 0.0f))));  // 오른쪽 시야 밖 (90°: |y| > x)
	E_EXPECT_TRUE(Frustum.Intersects(UnitBoxAt(FVector3(10.0f, 9.0f, 0.0f))));    // 시야 가장자리 안쪽
	E_EXPECT_FALSE(Frustum.Intersects(UnitBoxAt(FVector3(10.0f, 0.0f, 20.0f))));  // 위 시야 밖
	E_EXPECT_FALSE(Frustum.Intersects(UnitBoxAt(FVector3(150.0f, 0.0f, 0.0f))));  // 원평면 너머
	E_EXPECT_TRUE(Frustum.Intersects(UnitBoxAt(FVector3(1.0f, 0.0f, 0.0f))));     // 근평면에 걸침
	E_EXPECT_FALSE(Frustum.Intersects(UnitBoxAt(FVector3(0.2f, 0.0f, 0.0f))));    // 근평면 앞

	// 카메라를 감싸는 큰 상자는 항상 보임, 빈 상자는 보이지 않음
	E_EXPECT_TRUE(Frustum.Intersects(FBox(FVector3(-50.0f), FVector3(50.0f))));
	E_EXPECT_FALSE(Frustum.Intersects(FBox()));
}

// ---------------------------------------------------------------- 오일러 왕복 / 분해 / 반직선

E_TEST(Quat_ToEulerRoundtrip)
{
	const float Samples[][3] = {
		{ 0.0f, 0.0f, 0.0f }, { 30.0f, 0.0f, 0.0f }, { 0.0f, 120.0f, 0.0f }, { 0.0f, 0.0f, -45.0f },
		{ 20.0f, -70.0f, 130.0f }, { -60.0f, 170.0f, -170.0f }, { 89.0f, 10.0f, 20.0f },
	};
	for (const auto& Sample : Samples)
	{
		const FQuat Q = FQuat::FromEuler(Sample[0], Sample[1], Sample[2]);
		float Pitch, Yaw, Roll;
		Q.ToEuler(Pitch, Yaw, Roll);
		// 각도 자체가 아니라 회전이 같으면 된다 (동치 표현 허용)
		E_EXPECT_EQUALS(FQuat::FromEuler(Pitch, Yaw, Roll), Q, 1.0e-3f);
	}

	// 단순한 경우는 각도도 그대로 복원
	float Pitch, Yaw, Roll;
	FQuat::FromEuler(25.0f, 40.0f, -15.0f).ToEuler(Pitch, Yaw, Roll);
	E_EXPECT_NEAR(Pitch, 25.0f, 1.0e-2f);
	E_EXPECT_NEAR(Yaw, 40.0f, 1.0e-2f);
	E_EXPECT_NEAR(Roll, -15.0f, 1.0e-2f);
}

E_TEST(Matrix_Decompose)
{
	const FVector3 T(1.0f, -2.0f, 3.0f);
	const FQuat    R = FQuat::FromEuler(15.0f, 60.0f, -30.0f);
	const FVector3 S(2.0f, 0.5f, 3.0f);

	FVector3 OutT, OutS;
	FQuat    OutR;
	FMatrix4x4::MakeTransform(T, R, S).Decompose(OutT, OutR, OutS);
	E_EXPECT_EQUALS(OutT, T, Tol);
	E_EXPECT_EQUALS(OutS, S, 1.0e-3f);
	E_EXPECT_EQUALS(OutR, R, 1.0e-3f);
}

E_TEST(Ray_IntersectsBox)
{
	const FBox Box(FVector3(4.0f, -1.0f, -1.0f), FVector3(6.0f, 1.0f, 1.0f));
	float      Distance = 0.0f;

	E_EXPECT_TRUE(FRay(FVector3::ZeroVector, FVector3::ForwardVector).Intersects(Box, Distance));
	E_EXPECT_NEAR(Distance, 4.0f, Tol);
	E_EXPECT_FALSE(FRay(FVector3::ZeroVector, -FVector3::ForwardVector).Intersects(Box, Distance)); // 반대 방향
	E_EXPECT_FALSE(FRay(FVector3(0.0f, 5.0f, 0.0f), FVector3::ForwardVector).Intersects(Box, Distance)); // 옆으로 빗나감
	E_EXPECT_TRUE(FRay(FVector3(5.0f, 0.0f, 0.0f), FVector3::UpVector).Intersects(Box, Distance)); // 상자 안에서 시작
	E_EXPECT_NEAR(Distance, 0.0f, Tol);
	// 축 평행 광선이 슬랩 밖에서 출발
	E_EXPECT_FALSE(FRay(FVector3(0.0f, 0.0f, 5.0f), FVector3::ForwardVector).Intersects(Box, Distance));
}

E_TEST(Ray_FromNdcThroughCamera)
{
	const FMatrix4x4 View = FMatrix4x4::MakeLookAt(FVector3(-10.0f, 0.0f, 0.0f), FVector3::ZeroVector);
	const FMatrix4x4 Proj = FMatrix4x4::MakePerspectiveFov(FMath::DegreesToRadians(90.0f), 1.0f, 1.0f, 100.0f);
	const FMatrix4x4 InvViewProj = (View * Proj).GetInverse();

	// 화면 중앙 → 카메라 앞(+X) 방향
	const FRay Center = FRay::FromNdc(0.0f, 0.0f, InvViewProj);
	E_EXPECT_EQUALS(Center.Direction, FVector3::ForwardVector, 1.0e-3f);
	E_EXPECT_NEAR(Center.Origin.X, -9.0f, 1.0e-2f); // 근평면 위

	// 화면 오른쪽 가장자리(NDC x=1, FOV 90°) → 45° 오른쪽(+Y)
	const FRay Right = FRay::FromNdc(1.0f, 0.0f, InvViewProj);
	E_EXPECT_NEAR(Right.Direction.Y, Right.Direction.X, 1.0e-3f);
	E_EXPECT_TRUE(Right.Direction.Y > 0.0f);
	// 화면 위쪽 → +Z
	const FRay Up = FRay::FromNdc(0.0f, 1.0f, InvViewProj);
	E_EXPECT_TRUE(Up.Direction.Z > 0.0f);
}

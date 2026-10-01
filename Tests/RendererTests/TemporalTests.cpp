#include "Core/Testing/TestFramework.h"
#include "Renderer/Camera.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/TemporalMath.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace
{
	constexpr float Tol = 1.0e-4f;

	FVector2 ProjectToNdc(const FMatrix4x4& ViewProjection, const FVector3& World)
	{
		const FVector4 Clip = ViewProjection.TransformVector4(FVector4(World.X, World.Y, World.Z, 1.0f));
		return FVector2(Clip.X / Clip.W, Clip.Y / Clip.W);
	}

	FCamera MakeCamera(bool bOrthographic)
	{
		FCamera Camera;
		Camera.SetPosition(FVector3(-500.0f, 100.0f, 200.0f));
		Camera.LookAt(FVector3(0.0f, 0.0f, 0.0f));
		if (bOrthographic)
		{
			Camera.SetOrthographic(800.0f, 16.0f / 9.0f, 10.0f, 10000.0f);
		}
		else
		{
			Camera.SetPerspective(60.0f, 16.0f / 9.0f, 10.0f, 10000.0f);
		}
		return Camera;
	}
} // namespace

E_TEST(Temporal_HaltonSequence)
{
	E_EXPECT_NEAR(FTemporalMath::Halton(1, 2), 0.5f, Tol);
	E_EXPECT_NEAR(FTemporalMath::Halton(2, 2), 0.25f, Tol);
	E_EXPECT_NEAR(FTemporalMath::Halton(3, 2), 0.75f, Tol);
	E_EXPECT_NEAR(FTemporalMath::Halton(1, 3), 1.0f / 3.0f, Tol);
	E_EXPECT_NEAR(FTemporalMath::Halton(2, 3), 2.0f / 3.0f, Tol);
	E_EXPECT_NEAR(FTemporalMath::Halton(4, 3), 4.0f / 9.0f, Tol);
}

E_TEST(Temporal_JitterSamplesDistinctAndCentered)
{
	// 8개가 서로 다르고 [-0.5, 0.5) 안, 8 프레임마다 반복, 평균은 원점 근처
	std::vector<FVector2> Samples;
	FVector2              Sum;
	for (uint64 Frame = 0; Frame < FTemporalMath::JitterSampleCount; ++Frame)
	{
		const FVector2 Jitter = FTemporalMath::GetJitterPixels(Frame);
		E_EXPECT_TRUE(Jitter.X >= -0.5f && Jitter.X < 0.5f && Jitter.Y >= -0.5f && Jitter.Y < 0.5f);
		for (const FVector2& Other : Samples)
		{
			E_EXPECT_TRUE(FMath::Abs(Other.X - Jitter.X) > 1.0e-3f || FMath::Abs(Other.Y - Jitter.Y) > 1.0e-3f);
		}
		Samples.push_back(Jitter);
		Sum = Sum + Jitter;
	}
	E_EXPECT_TRUE(FMath::Abs(Sum.X / 8.0f) < 0.1f && FMath::Abs(Sum.Y / 8.0f) < 0.1f);
	const FVector2 Repeat = FTemporalMath::GetJitterPixels(FTemporalMath::JitterSampleCount);
	E_EXPECT_NEAR(Repeat.X, Samples[0].X, Tol);
	E_EXPECT_NEAR(Repeat.Y, Samples[0].Y, Tol);

	// 픽셀 → NDC: 1픽셀 = 2/W, 화면 아래(+Y 픽셀) = NDC -Y
	const FVector2 Ndc = FTemporalMath::JitterPixelsToNdc(FVector2(0.5f, 0.25f), 100, 50);
	E_EXPECT_NEAR(Ndc.X, 0.01f, Tol);
	E_EXPECT_NEAR(Ndc.Y, -0.01f, Tol);
}

E_TEST(Temporal_ProjectionJitterShiftsNdcExactly)
{
	// 원근/직교 모두: 지터 투영으로 투영한 점은 지터 없는 결과 + 오프셋 (깊이는 같다)
	const FVector2 Offset(0.013f, -0.007f);
	const FVector3 Points[] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(120.0f, -80.0f, 40.0f), FVector3(-200.0f, 300.0f, -50.0f) };
	for (int32 Ortho = 0; Ortho < 2; ++Ortho)
	{
		FCamera          Camera     = MakeCamera(Ortho != 0);
		const FMatrix4x4 Unjittered = Camera.GetViewProjectionMatrix();
		Camera.SetProjectionJitter(Offset);
		const FMatrix4x4 Jittered = Camera.GetViewProjectionMatrix();
		const FMatrix4x4 Explicit = Camera.GetUnjitteredViewProjectionMatrix();
		for (const FVector3& Point : Points)
		{
			const FVector2 A = ProjectToNdc(Unjittered, Point);
			const FVector2 B = ProjectToNdc(Jittered, Point);
			E_EXPECT_NEAR(B.X - A.X, Offset.X, Tol);
			E_EXPECT_NEAR(B.Y - A.Y, Offset.Y, Tol);
			const FVector4 ClipA = Unjittered.TransformVector4(FVector4(Point.X, Point.Y, Point.Z, 1.0f));
			const FVector4 ClipB = Jittered.TransformVector4(FVector4(Point.X, Point.Y, Point.Z, 1.0f));
			E_EXPECT_NEAR(ClipB.Z / ClipB.W, ClipA.Z / ClipA.W, Tol);
			const FVector2 C = ProjectToNdc(Explicit, Point);
			E_EXPECT_NEAR(C.X, A.X, Tol);
			E_EXPECT_NEAR(C.Y, A.Y, Tol);
		}
	}
}

E_TEST(Temporal_VelocityMatchesCameraReprojection)
{
	// 정지한 점: 움직임 벡터(현재 - 이전 UV) == 현재 UV - 재투영 UV (부호 규약 고정)
	FCamera Previous = MakeCamera(false);
	FCamera Current  = Previous;
	Current.SetPosition(Previous.GetPosition() + FVector3(10.0f, 25.0f, -5.0f));
	Current.LookAt(FVector3(20.0f, -10.0f, 0.0f));
	const FMatrix4x4 PrevVP       = Previous.GetViewProjectionMatrix();
	const FMatrix4x4 CurrVP       = Current.GetViewProjectionMatrix();
	const FMatrix4x4 Reprojection = FTemporalMath::ComputeReprojectionMatrix(CurrVP, PrevVP);

	const FVector3 Points[] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(100.0f, 50.0f, 30.0f), FVector3(-60.0f, -40.0f, 10.0f) };
	for (const FVector3& Point : Points)
	{
		const FVector4 CurrClip = CurrVP.TransformVector4(FVector4(Point.X, Point.Y, Point.Z, 1.0f));
		const FVector4 PrevClip = PrevVP.TransformVector4(FVector4(Point.X, Point.Y, Point.Z, 1.0f));
		const FVector2 Velocity = FTemporalMath::ComputeVelocity(CurrClip, PrevClip);

		const FVector2 CurrUv = FTemporalMath::NdcToUv(FVector2(CurrClip.X / CurrClip.W, CurrClip.Y / CurrClip.W));
		const FVector2 PrevUv = FTemporalMath::ReprojectUv(CurrUv, CurrClip.Z / CurrClip.W, Reprojection);
		E_EXPECT_NEAR(Velocity.X, CurrUv.X - PrevUv.X, 1.0e-3f);
		E_EXPECT_NEAR(Velocity.Y, CurrUv.Y - PrevUv.Y, 1.0e-3f);

		const FVector2 ExpectedPrevUv = FTemporalMath::NdcToUv(FVector2(PrevClip.X / PrevClip.W, PrevClip.Y / PrevClip.W));
		E_EXPECT_NEAR(PrevUv.X, ExpectedPrevUv.X, 1.0e-3f);
		E_EXPECT_NEAR(PrevUv.Y, ExpectedPrevUv.Y, 1.0e-3f);
	}

	// 카메라가 그대로면 움직임 0, 재투영은 같은 UV
	const FMatrix4x4 Same = FTemporalMath::ComputeReprojectionMatrix(CurrVP, CurrVP);
	const FVector2   Uv   = FTemporalMath::ReprojectUv(FVector2(0.3f, 0.7f), 0.5f, Same);
	E_EXPECT_NEAR(Uv.X, 0.3f, 1.0e-4f);
	E_EXPECT_NEAR(Uv.Y, 0.7f, 1.0e-4f);

	// 오른쪽으로 움직인 점 → 움직임 X 양수 (UV +X = 오른쪽)
	FCamera Fixed = MakeCamera(false);
	Fixed.SetPosition(FVector3(-500.0f, 0.0f, 0.0f));
	Fixed.LookAt(FVector3(0.0f, 0.0f, 0.0f));
	const FMatrix4x4 VP    = Fixed.GetViewProjectionMatrix();
	const FVector2   Right = FTemporalMath::ComputeVelocity(VP.TransformVector4(FVector4(0.0f, 10.0f, 0.0f, 1.0f)), VP.TransformVector4(FVector4(0.0f, 0.0f, 0.0f, 1.0f)));
	E_EXPECT_TRUE(Right.X > 0.0f);
	E_EXPECT_NEAR(Right.Y, 0.0f, 1.0e-5f);
	const FVector2 Up = FTemporalMath::ComputeVelocity(VP.TransformVector4(FVector4(0.0f, 0.0f, 10.0f, 1.0f)), VP.TransformVector4(FVector4(0.0f, 0.0f, 0.0f, 1.0f)));
	E_EXPECT_TRUE(Up.Y < 0.0f); // 위로 움직이면 UV Y 감소

	// w <= 0 (카메라 뒤)은 0
	const FVector2 Behind = FTemporalMath::ComputeVelocity(FVector4(1.0f, 1.0f, 1.0f, -1.0f), FVector4(0.0f, 0.0f, 0.0f, 1.0f));
	E_EXPECT_NEAR(Behind.X, 0.0f, Tol);
}

E_TEST(Temporal_OctahedralRoundTrip)
{
	const FVector3 Normals[] = { FVector3(0.0f, 0.0f, 1.0f),  FVector3(0.0f, 0.0f, -1.0f), FVector3(1.0f, 0.0f, 0.0f),
	                             FVector3(-1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f),  FVector3(0.0f, -1.0f, 0.0f),
	                             FVector3(0.3f, -0.5f, 0.8f).GetNormalized(), FVector3(-0.7f, 0.2f, -0.6f).GetNormalized(),
	                             FVector3(0.5f, 0.5f, -0.70710678f).GetNormalized() };
	for (const FVector3& Normal : Normals)
	{
		const FVector2 Encoded = FTemporalMath::EncodeOctahedral(Normal);
		E_EXPECT_TRUE(FMath::Abs(Encoded.X) <= 1.0f + Tol && FMath::Abs(Encoded.Y) <= 1.0f + Tol);
		const FVector3 Decoded = FTemporalMath::DecodeOctahedral(Encoded);
		E_EXPECT_NEAR(Decoded.X, Normal.X, 1.0e-4f);
		E_EXPECT_NEAR(Decoded.Y, Normal.Y, 1.0e-4f);
		E_EXPECT_NEAR(Decoded.Z, Normal.Z, 1.0e-4f);

		// R10G10B10A2 양자화 (10비트) 뒤에도 1도 안쪽
		const float    Steps = 1023.0f;
		const FVector2 Quantized(std::round((Encoded.X * 0.5f + 0.5f) * Steps) / Steps * 2.0f - 1.0f,
		                         std::round((Encoded.Y * 0.5f + 0.5f) * Steps) / Steps * 2.0f - 1.0f);
		const FVector3 Lossy = FTemporalMath::DecodeOctahedral(Quantized);
		E_EXPECT_TRUE(FVector3::Dot(Lossy, Normal) > FMath::Cos(FMath::DegreesToRadians(1.0f)));
	}
}

E_TEST(Temporal_CameraCut)
{
	const FVector3 Forward = FVector3::ForwardVector;
	E_EXPECT_FALSE(FTemporalMath::IsCameraCut(FVector3::ZeroVector, Forward, FVector3(100.0f, 0.0f, 0.0f), Forward, 2000.0f, 60.0f));
	E_EXPECT_TRUE(FTemporalMath::IsCameraCut(FVector3::ZeroVector, Forward, FVector3(2500.0f, 0.0f, 0.0f), Forward, 2000.0f, 60.0f));
	E_EXPECT_FALSE(FTemporalMath::IsCameraCut(FVector3::ZeroVector, Forward, FVector3::ZeroVector, FVector3(1.0f, 1.0f, 0.0f), 2000.0f, 60.0f)); // 45도
	E_EXPECT_TRUE(FTemporalMath::IsCameraCut(FVector3::ZeroVector, Forward, FVector3::ZeroVector, FVector3(0.0f, 1.0f, 0.0f), 2000.0f, 60.0f));  // 90도
}

E_TEST(Temporal_ShaderTypeLayout)
{
	// HLSL FInstanceData / PerFrame cbuffer와 같은 크기·위치
	E_EXPECT_EQ(sizeof(FInstanceGpuData), static_cast<size_t>(192));
	E_EXPECT_EQ(offsetof(FInstanceGpuData, BoneOffset), static_cast<size_t>(112));
	E_EXPECT_EQ(offsetof(FInstanceGpuData, PrevBoneOffset), static_cast<size_t>(116));
	E_EXPECT_EQ(offsetof(FInstanceGpuData, PrevWorld), static_cast<size_t>(128));
	E_EXPECT_EQ(offsetof(FPerFrameConstants, UnjitteredViewProjection), static_cast<size_t>(144));
	E_EXPECT_EQ(offsetof(FPerFrameConstants, PrevViewProjection), static_cast<size_t>(208));
	E_EXPECT_EQ(offsetof(FPerFrameConstants, JitterNdc), static_cast<size_t>(272));
}

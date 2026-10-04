#include "Core/Testing/TestFramework.h"
#include "Renderer/Camera.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/TemporalMath.h"

#include <algorithm>
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
	// 컴파일 타임 상수라 런타임 비교는 C4127(상수 조건식)이 되므로 static_assert로 고정
	static_assert(sizeof(FInstanceGpuData) == 192);
	static_assert(offsetof(FInstanceGpuData, BoneOffset) == 112);
	static_assert(offsetof(FInstanceGpuData, PrevBoneOffset) == 116);
	static_assert(offsetof(FInstanceGpuData, PrevWorld) == 128);
	static_assert(offsetof(FPerFrameConstants, UnjitteredViewProjection) == 144);
	static_assert(offsetof(FPerFrameConstants, PrevViewProjection) == 208);
	static_assert(offsetof(FPerFrameConstants, JitterNdc) == 272);
	E_EXPECT_TRUE(true);
}

E_TEST(Temporal_TaaColorSpaces)
{
	// 톤매핑 공간 왕복 + YCoCg 왕복
	const FVector3 Colors[] = { FVector3(0.0f), FVector3(0.2f, 0.5f, 0.9f), FVector3(4.0f, 1.0f, 0.25f), FVector3(30.0f, 30.0f, 30.0f) };
	for (const FVector3& Color : Colors)
	{
		const FVector3 Mapped = FTemporalMath::TonemapForTaa(Color);
		E_EXPECT_TRUE(Mapped.X < 1.0f && Mapped.Y < 1.0f && Mapped.Z < 1.0f);
		const FVector3 Back = FTemporalMath::InverseTonemapForTaa(Mapped);
		E_EXPECT_NEAR(Back.X, Color.X, Color.X * 1.0e-3f + 1.0e-4f);
		E_EXPECT_NEAR(Back.Z, Color.Z, Color.Z * 1.0e-3f + 1.0e-4f);
		const FVector3 RoundTrip = FTemporalMath::YCoCgToRgb(FTemporalMath::RgbToYCoCg(Mapped));
		E_EXPECT_NEAR(RoundTrip.X, Mapped.X, Tol);
		E_EXPECT_NEAR(RoundTrip.Y, Mapped.Y, Tol);
		E_EXPECT_NEAR(RoundTrip.Z, Mapped.Z, Tol);
	}
	// 회색은 Co = Cg = 0
	const FVector3 Gray = FTemporalMath::RgbToYCoCg(FVector3(0.4f));
	E_EXPECT_NEAR(Gray.X, 0.4f, Tol);
	E_EXPECT_NEAR(Gray.Y, 0.0f, Tol);
	E_EXPECT_NEAR(Gray.Z, 0.0f, Tol);
}

E_TEST(Temporal_TaaClipAndWeight)
{
	const FVector3 BoxMin(0.0f), BoxMax(1.0f);
	// 안쪽은 그대로
	const FVector3 Inside = FTemporalMath::ClipToBox(FVector3(0.2f, 0.7f, 0.5f), BoxMin, BoxMax);
	E_EXPECT_NEAR(Inside.X, 0.2f, Tol);
	E_EXPECT_NEAR(Inside.Y, 0.7f, Tol);
	// 바깥은 중심 방향 선분과 상자 경계의 교점
	const FVector3 Outside = FTemporalMath::ClipToBox(FVector3(2.5f, 0.5f, 0.5f), BoxMin, BoxMax);
	E_EXPECT_NEAR(Outside.X, 1.0f, 1.0e-3f);
	E_EXPECT_NEAR(Outside.Y, 0.5f, Tol);
	const FVector3 Corner = FTemporalMath::ClipToBox(FVector3(3.5f, 3.5f, 0.5f), BoxMin, BoxMax);
	E_EXPECT_NEAR(Corner.X, 1.0f, 1.0e-3f);
	E_EXPECT_NEAR(Corner.Y, 1.0f, 1.0e-3f);

	// 비중: 정지 + 반응형 없음 = 기본값, 반응형 1 = ReactiveWeight, 빠른 움직임 = 최소 0.25
	E_EXPECT_NEAR(FTemporalMath::ComputeTaaWeight(0.1f, 0.6f, 0.0f, 0.0f), 0.1f, Tol);
	E_EXPECT_NEAR(FTemporalMath::ComputeTaaWeight(0.1f, 0.6f, 1.0f, 0.0f), 0.6f, Tol);
	E_EXPECT_NEAR(FTemporalMath::ComputeTaaWeight(0.1f, 0.6f, 0.0f, 64.0f), 0.25f, Tol);
	E_EXPECT_NEAR(FTemporalMath::ComputeTaaWeight(0.1f, 0.6f, 0.5f, 0.0f), 0.35f, Tol);

	// 정지 화면 수렴: 같은 값을 계속 섞으면 그 값, 지터 샘플 평균으로 수렴 (지수 이동 평균)
	float History = 0.0f;
	for (int32 Frame = 0; Frame < 200; ++Frame)
	{
		const float Sample = (Frame % 2 == 0) ? 0.0f : 1.0f; // 가장자리 픽셀: 지터마다 덮임/안 덮임
		History            = FMath::Lerp(History, Sample, 0.1f);
	}
	E_EXPECT_NEAR(History, 0.5f, 0.06f);
}

namespace
{
	// 밝기 열을 통계에 흘려 Skip 이후 판정의 평균·최댓값 (TemporalAA.hlsl 깜빡임 감지와 같은 식)
	struct FFlickerRun
	{
		float Mean = 0.0f;
		float Max  = 0.0f;
	};
	FFlickerRun RunFlicker(const std::vector<float>& Lumas, size_t Skip)
	{
		FTemporalMath::FFlickerStats Stats = FTemporalMath::MakeFlickerStats(Lumas[0]);
		FFlickerRun                  Run;
		size_t                       Count = 0;
		for (size_t Index = 0; Index < Lumas.size(); ++Index)
		{
			const float Amount = FTemporalMath::ComputeFlickerAmount(Stats, Lumas[Index]);
			if (Index >= Skip)
			{
				Run.Mean += Amount;
				Run.Max = std::max(Run.Max, Amount);
				++Count;
			}
			Stats = FTemporalMath::UpdateFlickerStats(Stats, Lumas[Index]);
		}
		Run.Mean /= static_cast<float>(std::max<size_t>(Count, 1));
		return Run;
	}
} // namespace

// 지터로 오락가락하는 값(가는 기하 — Halton 8 주기 덮임)은 깜빡임으로, 정지·꾸준한 변화·느린 조명 흔들림·한 번에 바뀐 값은 아님
E_TEST(Temporal_FlickerDetectsJitterOscillation)
{
	constexpr size_t   Frames = 200;
	std::vector<float> Thin(Frames);
	std::vector<float> Alternate(Frames);
	for (size_t Index = 0; Index < Frames; ++Index)
	{
		Thin[Index]      = FTemporalMath::Halton(static_cast<uint32>(Index % 8) + 1, 2) > 0.5f ? 0.7f : 0.2f;
		Alternate[Index] = Index % 2 == 0 ? 0.2f : 0.6f;
	}
	E_EXPECT_TRUE(RunFlicker(Thin, 40).Mean > 0.95f);
	E_EXPECT_TRUE(RunFlicker(Alternate, 40).Mean > 0.95f);
}

E_TEST(Temporal_FlickerIgnoresRealChanges)
{
	constexpr size_t   Frames = 200;
	std::vector<float> Static(Frames, 0.4f);
	std::vector<float> Step(Frames);
	std::vector<float> Ramp(Frames);
	std::vector<float> SlowSine(Frames);
	for (size_t Index = 0; Index < Frames; ++Index)
	{
		Step[Index]     = Index < 50 ? 0.2f : 0.7f;
		Ramp[Index]     = 0.2f + 0.003f * static_cast<float>(Index);
		SlowSine[Index] = 0.4f + 0.2f * std::sin(6.2831853f * static_cast<float>(Index) / 30.0f); // 0.5초 주기 (60fps)
	}
	E_EXPECT_TRUE(RunFlicker(Static, 0).Max == 0.0f);
	E_EXPECT_TRUE(RunFlicker(Step, 0).Max == 0.0f); // 바뀐 순간·뒤 모두 (잔상 없음)
	E_EXPECT_TRUE(RunFlicker(Ramp, 0).Max == 0.0f);
	E_EXPECT_TRUE(RunFlicker(SlowSine, 40).Max == 0.0f);
	// 물체가 한 번 또는 드물게(0.5~1초마다 2~3프레임) 지나감 — 뒤집힘 2번이라 오락가락이 아니다 (잔상 줄무늬 회귀)
	std::vector<float> PassOnce(Frames, 0.6f);
	std::vector<float> PassEvery30(Frames, 0.6f);
	for (size_t Index = 0; Index < Frames; ++Index)
	{
		PassOnce[Index]    = (Index == 60 || Index == 61) ? 0.1f : 0.6f;
		PassEvery30[Index] = (Index > 20 && Index % 30 < 3) ? 0.1f : 0.6f;
	}
	E_EXPECT_TRUE(RunFlicker(PassOnce, 0).Max == 0.0f);
	E_EXPECT_TRUE(RunFlicker(PassEvery30, 0).Max == 0.0f);
	// 깜빡이던 화소에 다른 값이 들어오면(가려짐·한 번에 바뀜) 그 프레임 판정은 0
	FTemporalMath::FFlickerStats Stats = FTemporalMath::MakeFlickerStats(0.2f);
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Stats = FTemporalMath::UpdateFlickerStats(Stats, Index % 2 == 0 ? 0.2f : 0.4f);
	}
	E_EXPECT_TRUE(FTemporalMath::ComputeFlickerAmount(Stats, 0.3f) > 0.95f);
	E_EXPECT_NEAR(FTemporalMath::ComputeFlickerAmount(Stats, 0.95f), 0.0f, Tol);
}

#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <cmath>

// 시간 누적 효과(TAA 등) 공용 순수 계산: 서브픽셀 지터, 움직임 벡터, 재투영, 화면 공간 법선 인코딩.
// GPU 쪽 같은 식: Engine/Shaders/ScreenSpace.hlsli (테스트 TemporalTests)
//   규약: 움직임 벡터 = 현재 UV - 이전 UV (지터 없는 위치 기준). 이전 UV = 현재 UV - 움직임 벡터. UV 원점은 왼쪽 위(+Y 아래)
struct FTemporalMath
{
	static constexpr uint32 JitterSampleCount = 8; // Halton(2, 3) 8개 반복

	// 할턴 수열 (Index >= 1). Base 진법 자릿수를 소수점 뒤로 뒤집는다 → [0, 1)
	static float Halton(uint32 Index, uint32 Base)
	{
		float  Result   = 0.0f;
		float  Fraction = 1.0f;
		uint32 Value    = Index;
		while (Value > 0)
		{
			Fraction /= static_cast<float>(Base);
			Result += Fraction * static_cast<float>(Value % Base);
			Value /= Base;
		}
		return Result;
	}

	// 프레임 번호 → 픽셀 단위 지터 [-0.5, 0.5). 0번 프레임은 Halton 1번 (0은 원점이라 건너뛴다)
	static FVector2 GetJitterPixels(uint64 FrameIndex, uint32 SampleCount = JitterSampleCount)
	{
		const uint32 Index = static_cast<uint32>(FrameIndex % FMath::Max(SampleCount, 1u)) + 1;
		return FVector2(Halton(Index, 2) - 0.5f, Halton(Index, 3) - 0.5f);
	}

	// 픽셀 지터 → NDC 오프셋 (NDC +Y 위 = 화면 위, 픽셀 +Y 아래이므로 Y 부호 반전)
	static FVector2 JitterPixelsToNdc(const FVector2& Pixels, uint32 Width, uint32 Height)
	{
		return FVector2(2.0f * Pixels.X / static_cast<float>(FMath::Max(Width, 1u)), -2.0f * Pixels.Y / static_cast<float>(FMath::Max(Height, 1u)));
	}

	// 투영 행렬 뒤에 NDC 평행 이동을 곱한다: clip.xy += clip.w * Offset (원근/직교 공통, 행벡터 규약 v * M)
	static FMatrix4x4 ApplyProjectionJitter(const FMatrix4x4& Projection, const FVector2& NdcOffset)
	{
		FMatrix4x4 Result = Projection;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			Result.M[Row][0] += NdcOffset.X * Projection.M[Row][3];
			Result.M[Row][1] += NdcOffset.Y * Projection.M[Row][3];
		}
		return Result;
	}

	// NDC xy → UV (왼쪽 위 원점)
	static FVector2 NdcToUv(const FVector2& Ndc) { return FVector2(Ndc.X * 0.5f + 0.5f, 0.5f - Ndc.Y * 0.5f); }
	static FVector2 UvToNdc(const FVector2& Uv) { return FVector2(Uv.X * 2.0f - 1.0f, 1.0f - Uv.Y * 2.0f); }

	// 클립 좌표 두 개(현재/이전, 지터 없음) → 움직임 벡터 (UV 단위, 현재 - 이전). w <= 0이면 0
	static FVector2 ComputeVelocity(const FVector4& CurrentClip, const FVector4& PreviousClip)
	{
		if (CurrentClip.W <= 1.0e-6f || PreviousClip.W <= 1.0e-6f)
		{
			return FVector2::ZeroVector;
		}
		const FVector2 Current(CurrentClip.X / CurrentClip.W, CurrentClip.Y / CurrentClip.W);
		const FVector2 Previous(PreviousClip.X / PreviousClip.W, PreviousClip.Y / PreviousClip.W);
		return FVector2((Current.X - Previous.X) * 0.5f, (Previous.Y - Current.Y) * 0.5f);
	}

	// 카메라 움직임만의 재투영 행렬: 현재 클립(지터 없음) → 이전 클립 = (현재 VP)⁻¹ * 이전 VP
	static FMatrix4x4 ComputeReprojectionMatrix(const FMatrix4x4& CurrentViewProjection, const FMatrix4x4& PreviousViewProjection)
	{
		return CurrentViewProjection.GetInverse() * PreviousViewProjection;
	}

	// 정지한 점의 이전 UV: 현재 UV + 깊이(NDC z) → 재투영 행렬 → 이전 UV
	static FVector2 ReprojectUv(const FVector2& Uv, float DeviceDepth, const FMatrix4x4& Reprojection)
	{
		const FVector2 Ndc = UvToNdc(Uv);
		const FVector4 Previous = Reprojection.TransformVector4(FVector4(Ndc.X, Ndc.Y, DeviceDepth, 1.0f));
		if (FMath::Abs(Previous.W) <= 1.0e-8f)
		{
			return Uv;
		}
		return NdcToUv(FVector2(Previous.X / Previous.W, Previous.Y / Previous.W));
	}

	// 카메라 컷 판정: 위치가 MaxMove(cm) 넘게 바뀌었거나 앞 방향이 MaxAngleDegrees 넘게 돌았으면 이력 리셋
	static bool IsCameraCut(const FVector3& PreviousPosition, const FVector3& PreviousForward, const FVector3& Position, const FVector3& Forward,
	                        float MaxMove, float MaxAngleDegrees)
	{
		if (FVector3::DistanceSquared(PreviousPosition, Position) > MaxMove * MaxMove)
		{
			return true;
		}
		const float CosAngle = FVector3::Dot(PreviousForward.GetNormalized(), Forward.GetNormalized());
		return CosAngle < FMath::Cos(FMath::DegreesToRadians(MaxAngleDegrees));
	}

	// ---- TAA 해상 (TemporalAA.hlsl과 같은 식)
	// 밝은 픽셀이 이력을 지배하지 않게 섞는 공간: c / (1 + max(c)), 역변환 c / (1 - max(c))
	static FVector3 TonemapForTaa(const FVector3& C)
	{
		return C / (1.0f + FMath::Max(C.X, FMath::Max(C.Y, C.Z)));
	}
	static FVector3 InverseTonemapForTaa(const FVector3& C)
	{
		return C / FMath::Max(1.0f - FMath::Max(C.X, FMath::Max(C.Y, C.Z)), 1.0e-4f);
	}
	static FVector3 RgbToYCoCg(const FVector3& C)
	{
		return FVector3(0.25f * C.X + 0.5f * C.Y + 0.25f * C.Z, 0.5f * C.X - 0.5f * C.Z, -0.25f * C.X + 0.5f * C.Y - 0.25f * C.Z);
	}
	static FVector3 YCoCgToRgb(const FVector3& C) { return FVector3(C.X + C.Y - C.Z, C.X + C.Z, C.X - C.Y - C.Z); }

	// 이력을 상자 중심 방향으로 상자 안까지 당긴다 (안이면 그대로)
	static FVector3 ClipToBox(const FVector3& History, const FVector3& BoxMin, const FVector3& BoxMax)
	{
		const FVector3 Center = (BoxMax + BoxMin) * 0.5f;
		const FVector3 Extent = (BoxMax - BoxMin) * 0.5f + FVector3(1.0e-5f);
		const FVector3 Offset = History - Center;
		const float    Scale  = FMath::Max(FMath::Abs(Offset.X / Extent.X), FMath::Max(FMath::Abs(Offset.Y / Extent.Y), FMath::Abs(Offset.Z / Extent.Z)));
		return Scale > 1.0f ? Center + Offset / Scale : History;
	}

	// 현재 프레임 비중: 기본값 → 반응형 마스크(0~1)만큼 ReactiveWeight로, 빠른 움직임(32픽셀에서 최대)이면 최소 0.25
	static float ComputeTaaWeight(float CurrentWeight, float ReactiveWeight, float Reactive, float SpeedPixels)
	{
		const float Base = FMath::Lerp(CurrentWeight, ReactiveWeight, FMath::Clamp(Reactive, 0.0f, 1.0f));
		return FMath::Lerp(Base, FMath::Max(Base, 0.25f), FMath::Clamp(SpeedPixels / 32.0f, 0.0f, 1.0f));
	}

	// 단위 법선 → 팔면체 [-1, 1]^2 (화면 공간 법선 버퍼 RG). Engine/Shaders/ScreenSpace.hlsli와 같은 식
	static FVector2 EncodeOctahedral(const FVector3& Normal)
	{
		const float L1 = FMath::Abs(Normal.X) + FMath::Abs(Normal.Y) + FMath::Abs(Normal.Z);
		FVector2    P(Normal.X / FMath::Max(L1, 1.0e-8f), Normal.Y / FMath::Max(L1, 1.0e-8f));
		if (Normal.Z < 0.0f)
		{
			const FVector2 Folded((1.0f - FMath::Abs(P.Y)) * (P.X >= 0.0f ? 1.0f : -1.0f), (1.0f - FMath::Abs(P.X)) * (P.Y >= 0.0f ? 1.0f : -1.0f));
			P = Folded;
		}
		return P;
	}

	static FVector3 DecodeOctahedral(const FVector2& Encoded)
	{
		FVector3    N(Encoded.X, Encoded.Y, 1.0f - FMath::Abs(Encoded.X) - FMath::Abs(Encoded.Y));
		const float T = FMath::Clamp(-N.Z, 0.0f, 1.0f);
		N.X += N.X >= 0.0f ? -T : T;
		N.Y += N.Y >= 0.0f ? -T : T;
		return N.GetNormalized();
	}
};

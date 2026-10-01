#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/MathUtils.h"

#include <cmath>

// SSAO (GTAO 방식, Jimenez 2016 "Practical Real-Time Strategies for Accurate Indirect Occlusion") CPU 측 순수 계산.
// GPU 쪽 같은 식: Engine/Shaders/AmbientOcclusion.hlsl (테스트 AmbientOcclusionTests)
//   슬라이스(화면 방향 하나 + 시선)마다 양쪽 지평선 각 h0(음수 쪽), h1(양수 쪽)과 법선의 슬라이스 투영 각 n으로
//   코사인 가중 가시도 = IntegrateArc(h0, n) + IntegrateArc(h1, n) (가림 없음 = 1). 각도는 시선 기준 [-π/2, π/2]
struct FAmbientOcclusionMath
{
	static constexpr uint32 SliceCount = 2; // 픽셀당 방향 수 (TAA가 프레임마다 회전해 누적)
	static constexpr uint32 StepCount  = 4; // 방향 한쪽당 샘플 수

	// 지평선 각 H까지 열린 호의 코사인 가중 적분 (법선 투영 각 N 기준)
	static float IntegrateArc(float H, float N) { return 0.25f * (-std::cos(2.0f * H - N) + std::cos(N) + 2.0f * H * std::sin(N)); }

	// 슬라이스 가시도: 지평선 코사인(시선과의) 두 개 → 각 → 법선 반구로 제한 → 적분
	static float ComputeSliceVisibility(float CosHorizonNegative, float CosHorizonPositive, float NormalAngle)
	{
		const float HalfPi = FMath::Pi * 0.5f;
		float       H0     = -std::acos(FMath::Clamp(CosHorizonNegative, -1.0f, 1.0f));
		float       H1     = std::acos(FMath::Clamp(CosHorizonPositive, -1.0f, 1.0f));
		H0                 = NormalAngle + FMath::Max(H0 - NormalAngle, -HalfPi);
		H1                 = NormalAngle + FMath::Min(H1 - NormalAngle, HalfPi);
		return IntegrateArc(H0, NormalAngle) + IntegrateArc(H1, NormalAngle);
	}

	// 거리 감쇠: 반경 안에서 1 → 반경에서 0 (지평선 코사인을 -1 쪽으로 당긴다)
	static float ComputeFalloff(float DistanceSquared, float Radius)
	{
		return FMath::Clamp(1.0f - DistanceSquared / FMath::Max(Radius * Radius, 1.0e-4f), 0.0f, 1.0f);
	}

	// 월드 반경(cm) → 화면 픽셀 반경. ProjectionYScale = 투영 행렬 M[1][1], 원근이면 뷰 깊이로 나눈다
	static float GetRadiusPixels(float Radius, float ViewDepth, float ProjectionYScale, float ScreenHeight, bool bOrthographic)
	{
		const float PixelsPerUnit = 0.5f * ScreenHeight * ProjectionYScale;
		return Radius * PixelsPerUnit / (bOrthographic ? 1.0f : FMath::Max(ViewDepth, 1.0e-3f));
	}

	// 양방향 블러/업샘플 가중: 깊이 상대 차이가 클수록 0에 가깝다 (Sharpness가 클수록 경계 보존)
	static float ComputeDepthWeight(float CenterDepth, float SampleDepth, float Sharpness)
	{
		const float Relative = FMath::Abs(SampleDepth - CenterDepth) / FMath::Max(CenterDepth, 1.0e-3f);
		return std::exp(-Relative * Sharpness);
	}

	// 세기 적용: AO^Intensity (1 = 그대로)
	static float ApplyIntensity(float Visibility, float Intensity)
	{
		return std::pow(FMath::Clamp(Visibility, 0.0f, 1.0f), FMath::Max(Intensity, 0.0f));
	}
};

#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/MathUtils.h"

#include <cmath>

// 포스트 프로세싱의 CPU 측 순수 계산 (GPU 셰이더와 같은 식, 단위 테스트 대상)
struct FPostProcessMath
{
	static constexpr uint32 MaxBloomMips   = 6;
	static constexpr uint32 MinBloomMipDim = 4; // 이보다 작은 레벨은 만들지 않는다

	// 자동 노출 히스토그램의 로그2 휘도 범위
	static constexpr float  HistogramMinLog2Luminance = -10.0f;
	static constexpr float  HistogramMaxLog2Luminance = 12.0f;
	static constexpr uint32 HistogramBinCount         = 256; // 0번 칸은 거의 검은 픽셀
	static constexpr uint32 HistogramDownscale        = 4;   // 히스토그램은 1/4 해상도로 샘플링

	// 노출 기준: 평균 휘도를 18% 회색으로 맞춘다
	static constexpr float MiddleGray = 0.18f;

	// 블룸 레벨 i(0부터)의 크기: 절반 해상도에서 시작해 레벨마다 절반
	static uint32 GetBloomMipDimension(uint32 FullDimension, uint32 Level)
	{
		const uint32 Shift = Level + 1;
		return Shift >= 32 ? 1u : FMath::Max(1u, FullDimension >> Shift);
	}

	// 짧은 변이 MinBloomMipDim 이상인 레벨만, 최대 MaxBloomMips개
	static uint32 GetBloomMipCount(uint32 Width, uint32 Height)
	{
		uint32 Count = 0;
		while (Count < MaxBloomMips)
		{
			const uint32 W = GetBloomMipDimension(Width, Count);
			const uint32 H = GetBloomMipDimension(Height, Count);
			if (FMath::Min(W, H) < MinBloomMipDim)
			{
				break;
			}
			++Count;
		}
		return Count;
	}

	// 지수 적응: Speed(1/초)가 클수록 빨리 목표에 도달. DeltaSeconds <= 0이면 이전 값 유지
	static float ComputeAdaptedLuminance(float Previous, float Target, float DeltaSeconds, float Speed)
	{
		if (!(Previous > 0.0f) || !std::isfinite(Previous))
		{
			return Target; // 첫 프레임 또는 잘못된 이전 값
		}
		const float Alpha = 1.0f - std::exp(-FMath::Max(DeltaSeconds, 0.0f) * FMath::Max(Speed, 0.0f));
		return Previous + (Target - Previous) * Alpha;
	}

	// 평균 휘도 → 자동 노출 EV (MiddleGray 기준), [MinEV, MaxEV]로 제한
	static float ComputeAutoExposureEV(float AverageLuminance, float MinEV, float MaxEV)
	{
		const float EV = std::log2(MiddleGray / FMath::Max(AverageLuminance, 1.0e-4f));
		return FMath::Clamp(EV, MinEV, MaxEV);
	}

	// 블룸 임계값 소프트 니: 임계값 부근을 2차 곡선으로 부드럽게 (Unity/UE 방식). 반환: 기여 배율 [0, 1]
	static float ComputeBloomContribution(float Brightness, float Threshold, float Knee)
	{
		const float SoftKnee = Threshold * Knee + 1.0e-5f;
		float       Soft     = FMath::Clamp(Brightness - Threshold + SoftKnee, 0.0f, 2.0f * SoftKnee);
		Soft                 = Soft * Soft / (4.0f * SoftKnee);
		const float Contrib  = FMath::Max(Soft, Brightness - Threshold);
		return Contrib / FMath::Max(Brightness, 1.0e-5f);
	}

	// ---- 피사계 심도 (DepthOfField.hlsl ComputeCoc와 같은 식)
	// 착란원(CoC) 반경 = 화면 높이 비율, 부호: 음수 = 초점 앞(근경), 양수 = 초점 뒤(원경).
	//   [초점 - 영역, 초점 + 영역]은 0(선명), 그 밖은 전환 거리 동안 선형으로 커져 근경/원경 최대 반경에서 멈춘다 (UE 가우시안 DOF 방식 매개변수)
	//   전환 거리 <= 0이면 영역을 벗어나자마자 최대
	static float ComputeCircleOfConfusion(float ViewDepth, float FocusDistance, float FocalRegion, float NearTransition, float FarTransition,
	                                      float NearBlur, float FarBlur)
	{
		const float Region = FMath::Max(FocalRegion, 0.0f);
		const float Near   = FocusDistance - Region - ViewDepth; // > 0이면 근경
		const float Far    = ViewDepth - FocusDistance - Region;  // > 0이면 원경
		if (Near > 0.0f)
		{
			const float T = NearTransition > 0.0f ? FMath::Min(Near / NearTransition, 1.0f) : 1.0f;
			return -T * FMath::Max(NearBlur, 0.0f);
		}
		if (Far > 0.0f)
		{
			const float T = FarTransition > 0.0f ? FMath::Min(Far / FarTransition, 1.0f) : 1.0f;
			return T * FMath::Max(FarBlur, 0.0f);
		}
		return 0.0f;
	}

	// 장치 깊이 [0, 1] → 뷰 깊이(cm). 원근은 표준 깊이(근평면 0, 원평면 1), 직교는 선형 (PixelArt.hlsl LinearizeDepth와 같음)
	static float LinearizeDepth(float DeviceDepth, float NearZ, float FarZ, bool bOrthographic)
	{
		if (bOrthographic)
		{
			return NearZ + (FarZ - NearZ) * DeviceDepth;
		}
		return NearZ * FarZ / (FarZ - DeviceDepth * (FarZ - NearZ));
	}

	// 피사계 심도 반해상도 버퍼 크기 (홀수는 올림)
	static uint32 GetDepthOfFieldDimension(uint32 FullDimension) { return FMath::Max(1u, (FullDimension + 1) / 2); }
};

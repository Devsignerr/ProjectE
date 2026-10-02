#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <cmath>

// TAAU (시간 업샘플, Phase 48) 순수 계산. GPU 쪽 같은 식: Engine/Shaders/TemporalAA.hlsl PSResolveUpsample (테스트 UpscaleTests)
//   - 씬은 내부 해상도(출력 × 화면 비율)로 그리고 TAA 단계가 출력 해상도 이력에 누적한다
//   - 지터는 내부 해상도 픽셀 기준 (Halton 2,3), 표본 수는 출력/내부 면적비에 비례해 늘린다 (출력 픽셀마다 가까운 표본이 고르게 오도록)
//   - 머티리얼 텍스처 밉 바이어스 = log2(내부/출력) + 보정 → 출력 해상도에서 본 것과 같은 텍스처 선명도
struct FUpscaleMath
{
	static constexpr float MinScreenPercentage = 25.0f;
	static constexpr float MaxScreenPercentage = 100.0f;

	// 품질 프리셋 (EResolutionQuality 번호 — Core/GameUserSettings.h): 네이티브 100 / 품질 77 / 균형 67 / 성능 50
	static float GetPresetScreenPercentage(int32 Preset)
	{
		switch (Preset)
		{
		case 1:  return 77.0f;
		case 2:  return 67.0f;
		case 3:  return 50.0f;
		default: return 100.0f;
		}
	}

	static float ClampScreenPercentage(float Percentage)
	{
		if (!(Percentage == Percentage)) // NaN
		{
			return MaxScreenPercentage;
		}
		return FMath::Clamp(Percentage, MinScreenPercentage, MaxScreenPercentage);
	}

	// 출력 크기 × 화면 비율 → 내부 크기 (반올림, 1 이상, 출력 이하)
	static uint32 ComputeInternalDimension(uint32 OutputDimension, float Percentage)
	{
		if (OutputDimension == 0)
		{
			return 0;
		}
		const float  Scaled = static_cast<float>(OutputDimension) * ClampScreenPercentage(Percentage) / 100.0f;
		const uint32 Value  = static_cast<uint32>(std::floor(Scaled + 0.5f));
		return FMath::Clamp(Value, 1u, OutputDimension);
	}

	// 화면 비율을 Step(%) 단위로 반올림 (동적 해상도 단계 — 같은 단계면 같은 내부 크기 → 리소스 재할당 없음)
	static float QuantizePercentage(float Percentage, float Step)
	{
		if (Step <= 0.0f)
		{
			return Percentage;
		}
		return std::floor(Percentage / Step + 0.5f) * Step;
	}

	// 텍스처 밉 바이어스: 내부가 출력보다 작을 때만 log2(내부/출력) + Offset (Offset은 음수 쪽 = 더 선명). 같거나 크면 0 (네이티브와 비트 동일)
	static float ComputeMipBias(uint32 InternalHeight, uint32 OutputHeight, float Offset)
	{
		if (InternalHeight == 0 || OutputHeight == 0 || InternalHeight >= OutputHeight)
		{
			return 0.0f;
		}
		return std::log2(static_cast<float>(InternalHeight) / static_cast<float>(OutputHeight)) + Offset;
	}

	// 지터 표본 수: 8 × (출력/내부)² (면적비), 8~64. 내부 = 출력이면 기존 TAA와 같은 8
	static uint32 GetJitterSampleCount(uint32 InternalHeight, uint32 OutputHeight)
	{
		if (InternalHeight == 0 || InternalHeight >= OutputHeight)
		{
			return 8;
		}
		const float Ratio = static_cast<float>(OutputHeight) / static_cast<float>(InternalHeight);
		const float Count = std::ceil(8.0f * Ratio * Ratio);
		return FMath::Clamp(static_cast<uint32>(Count), 8u, 64u);
	}

	// NDC 지터 → UV 지터 (지터된 영상에서 지터 없는 위치 Uv의 점은 Uv + 이 값에 보인다. UV +Y 아래이므로 Y 부호 반전)
	static FVector2 JitterNdcToUv(const FVector2& JitterNdc) { return FVector2(JitterNdc.X * 0.5f, -JitterNdc.Y * 0.5f); }

	// 업샘플 표본 가중치: 출력 픽셀 중심과 내부 표본 사이 거리(출력 픽셀 단위)의 가우시안 (블랙맨-해리스 근사 exp(-2.29 d²))
	static float SampleWeight(float DistanceSquaredOutputPixels) { return std::exp(-2.29f * DistanceSquaredOutputPixels); }

	// 출력 픽셀 중심 Uv(지터 없음)에 대해: 지터된 내부 영상에서 그 점이 놓인 내부 픽셀 좌표(연속, 픽셀 중심 = 정수 + 0.5)
	static FVector2 GetInputPosition(const FVector2& OutputUv, const FVector2& JitterUv, uint32 InternalWidth, uint32 InternalHeight)
	{
		return FVector2((OutputUv.X + JitterUv.X) * static_cast<float>(InternalWidth), (OutputUv.Y + JitterUv.Y) * static_cast<float>(InternalHeight));
	}
};

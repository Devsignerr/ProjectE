#pragma once

#include "Core/Math/MathUtils.h"

// 메시 LOD 선택 식 (순수 함수 — RendererTests의 LodTests)
//   화면 크기(ScreenSize) = 경계 구 지름이 화면 높이에서 차지하는 비율 (언리얼 ScreenSize와 같은 뜻, 1 = 화면 높이만큼)
//   LOD i(≥1)는 화면 크기가 Thresholds[i]보다 작을 때 쓴다. Thresholds는 LOD0 = 1부터 내림차순
namespace LodMath
{
	constexpr uint32 MaxLods = 4; // LOD0 + 단순화 3단계

	// 임포트 기본값: LOD마다 화면 크기 임계값과 원본 대비 삼각형 비율
	constexpr float DefaultScreenSizes[MaxLods]     = { 1.0f, 0.5f, 0.25f, 0.12f };
	constexpr float DefaultTriangleRatios[MaxLods]  = { 1.0f, 0.5f, 0.25f, 0.1f };

	// 원근: TanHalfFovY = tan(세로 시야각 / 2). 카메라가 구 안에 있으면 큰 값(→ LOD0)
	inline float ComputePerspectiveScreenSize(float Radius, float Distance, float TanHalfFovY)
	{
		if (Distance <= Radius || TanHalfFovY <= 0.0f)
		{
			return 1.0e6f;
		}
		return Radius / (Distance * TanHalfFovY);
	}

	// 직교: 화면 높이 = OrthoHeight (월드 단위)
	inline float ComputeOrthographicScreenSize(float Radius, float OrthoHeight)
	{
		return OrthoHeight > 0.0f ? 2.0f * Radius / OrthoHeight : 1.0e6f;
	}

	// Scale > 1이면 고품질 쪽(LOD 전환을 더 멀리), < 1이면 더 일찍 낮은 LOD
	inline uint32 SelectLod(float ScreenSize, const float* Thresholds, uint32 LodCount, float Scale = 1.0f)
	{
		const float Size = ScreenSize * FMath::Max(Scale, 0.0f);
		uint32      Lod  = 0;
		for (uint32 Index = 1; Index < LodCount && Index < MaxLods; ++Index)
		{
			if (Size >= Thresholds[Index])
			{
				break;
			}
			Lod = Index;
		}
		return Lod;
	}
} // namespace LodMath

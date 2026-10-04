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

	// 메인/사전 패스 인스턴스 컬링 (r.MinScreenSize, r.MaxDrawDistance): 화면 크기가 MinScreenSize보다 작거나
	// 경계 구 표면까지 거리(SurfaceDistance)가 MaxDrawDistance보다 멀면 true. 각각 0 이하 = 끔
	inline bool ShouldCullInstance(float ScreenSize, float SurfaceDistance, float MinScreenSize, float MaxDrawDistance)
	{
		return (MinScreenSize > 0.0f && ScreenSize < MinScreenSize) || (MaxDrawDistance > 0.0f && SurfaceDistance > MaxDrawDistance);
	}

	// ---- 오차 기반 선택 (언리얼 자동 LOD·나나이트와 같은 생각): 임포트가 저장한 LOD별 형상 오차(로컬 단위, FMeshLod::Error)를
	//   화면 픽셀로 투영해 허용치(r.LOD.ErrorPixels) 안에서 가장 거친 LOD를 고른다 → 바뀌는 순간 모양 차이가 그 픽셀 이하.
	//   PixelsPerUnit = 그 거리에서 월드 1단위가 차지하는 화면 픽셀 × 월드 스케일. Errors[0] = 0, LOD 번호에 단조 증가

	// 원근: 화면 세로 픽셀 / (2 × 거리 × tan(세로 시야각/2)). 거리 ≤ 0(카메라가 경계 안)이면 아주 큰 값(→ LOD0)
	inline float ComputePerspectivePixelsPerUnit(float Distance, float TanHalfFovY, float ViewportHeight)
	{
		if (Distance <= 0.0f || TanHalfFovY <= 0.0f)
		{
			return 1.0e9f;
		}
		return ViewportHeight * 0.5f / (Distance * TanHalfFovY);
	}

	// 직교: 화면 세로 픽셀 / OrthoHeight
	inline float ComputeOrthographicPixelsPerUnit(float OrthoHeight, float ViewportHeight)
	{
		return OrthoHeight > 0.0f ? ViewportHeight / OrthoHeight : 1.0e9f;
	}

	// 투영 오차가 MaxErrorPixels 이하인 가장 거친 LOD
	inline uint32 SelectLodByError(const float* Errors, uint32 LodCount, float PixelsPerUnit, float MaxErrorPixels)
	{
		uint32 Lod = 0;
		for (uint32 Index = 1; Index < LodCount && Index < MaxLods; ++Index)
		{
			if (Errors[Index] * PixelsPerUnit > MaxErrorPixels)
			{
				break;
			}
			Lod = Index;
		}
		return Lod;
	}

	// 히스테리시스 범위 (SelectLodWithHysteresis와 같은 뜻): Fine = 허용치 × (1 - H)로 고른 LOD(이전이 이보다 고우면 여기까지 내려간다),
	//   Coarse = 허용치 × (1 + H)로 고른 LOD(이전이 이보다 거칠면 여기까지 올라간다). 결과 = clamp(이전, Fine, Coarse)
	struct FLodRangeByError
	{
		uint32 Fine   = 0;
		uint32 Coarse = 0;
		uint32 Exact  = 0; // 히스테리시스 없이 (이전 LOD가 없을 때)
	};
	inline FLodRangeByError SelectLodRangeByError(const float* Errors, uint32 LodCount, float PixelsPerUnit, float MaxErrorPixels, float Hysteresis)
	{
		const float H = FMath::Clamp(Hysteresis, 0.0f, 0.9f);
		return { SelectLodByError(Errors, LodCount, PixelsPerUnit, MaxErrorPixels * (1.0f - H)),
		         SelectLodByError(Errors, LodCount, PixelsPerUnit, MaxErrorPixels * (1.0f + H)),
		         SelectLodByError(Errors, LodCount, PixelsPerUnit, MaxErrorPixels) };
	}

	constexpr float DefaultHysteresis = 0.1f; // 전환 여유: 임계값의 ±10% 띠 안에서는 이전 LOD 유지

	// 히스테리시스 선택 (팝핑 완화): 임계값 T마다 [T × (1 - H), T × (1 + H)) 띠 안에서는 이전 LOD를 유지한다.
	//   낮은 품질로 내려가려면 화면 크기 < T × (1 - H), 높은 품질로 올라가려면 >= T × (1 + H).
	//   = clamp(이전, SelectLod(크기 / (1 - H)), SelectLod(크기 / (1 + H))). PreviousLod >= LodCount(처음 보는 인스턴스)면 SelectLod 그대로
	inline uint32 SelectLodWithHysteresis(float ScreenSize, const float* Thresholds, uint32 LodCount, float Scale, uint32 PreviousLod,
	                                      float Hysteresis = DefaultHysteresis)
	{
		const float H = FMath::Clamp(Hysteresis, 0.0f, 0.9f);
		if (PreviousLod >= LodCount || H <= 0.0f)
		{
			return SelectLod(ScreenSize, Thresholds, LodCount, Scale);
		}
		const uint32 Finest   = SelectLod(ScreenSize / (1.0f - H), Thresholds, LodCount, Scale); // 이보다 높은 품질은 유지 불가
		const uint32 Coarsest = SelectLod(ScreenSize / (1.0f + H), Thresholds, LodCount, Scale); // 이보다 낮은 품질은 유지 불가
		return FMath::Clamp(PreviousLod, Finest, Coarsest);
	}
} // namespace LodMath

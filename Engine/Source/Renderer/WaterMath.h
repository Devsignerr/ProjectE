#pragma once

#include "Core/Math/Math.h"
#include "Core/Math/Units.h"

#include <cmath>

// 소규모 물 (Phase 49, FWaterBodyComponent) — CPU 참조 식. 셰이더 Water.hlsl과 같은 식 (테스트 WaterTests)
//   물 볼륨 = 엔티티 위치 중심 상자 (Size 전체 크기, 회전은 요(Z)만), 수면 = 상자 윗면
//   굴절 색 = 바닥 색 × T + 산란 색 × 조명 × (1 - T), T = exp(-흡수(1/m) × 물속 거리(m)) — 채널별
//   반사 비율 = 슐릭 프레넬 (F0 = 0.02), 가장자리 거품 = 물 두께가 FoamDistance보다 얇은 곳
//   물속 카메라: 시선이 물속을 지나는 길이로 같은 흡수식 (상자 안 구간 ∩ [0, 장면 깊이])
namespace FWaterMath
{
	constexpr float WaterF0 = 0.02f; // 물 굴절률 1.33

	inline FVector3 Transmittance(const FVector3& AbsorptionPerMeter, float DistanceCentimeters)
	{
		const float Meters = FMath::Max(DistanceCentimeters, 0.0f) * FUnits::UnitsToMeters;
		return FVector3(std::exp(-AbsorptionPerMeter.X * Meters), std::exp(-AbsorptionPerMeter.Y * Meters), std::exp(-AbsorptionPerMeter.Z * Meters));
	}

	// 바닥(굴절) 색과 물 산란 색 섞기
	inline FVector3 ApplyAbsorption(const FVector3& Behind, const FVector3& ScatterColor, const FVector3& AbsorptionPerMeter, float DistanceCentimeters)
	{
		const FVector3 T = Transmittance(AbsorptionPerMeter, DistanceCentimeters);
		return Behind * T + ScatterColor * (FVector3::OneVector - T);
	}

	inline float FresnelSchlick(float CosTheta, float F0 = WaterF0)
	{
		const float C = 1.0f - FMath::Clamp(CosTheta, 0.0f, 1.0f);
		const float C2 = C * C;
		return F0 + (1.0f - F0) * C2 * C2 * C;
	}

	// 가장자리 거품 비중 (물 두께 cm 기준, 0 두께에서 1)
	inline float EdgeFoam(float ThicknessCentimeters, float FoamDistance)
	{
		if (FoamDistance <= 0.0f)
		{
			return 0.0f;
		}
		const float X = FMath::Clamp(1.0f - ThicknessCentimeters / FoamDistance, 0.0f, 1.0f);
		return X * X;
	}

	// 굴절 오프셋 배율: 얕은 물에서는 줄인다 (두께 0에서 0, RefractionDepth 이상에서 1)
	inline float RefractionScale(float ThicknessCentimeters, float RefractionDepth)
	{
		return FMath::Clamp(ThicknessCentimeters / FMath::Max(RefractionDepth, 1.0f), 0.0f, 1.0f);
	}

	// 광선 - 상자(로컬 축 정렬, 반 크기 HalfExtents) 구간 [Near, Far]. 원점이 안이면 Near = 0. 없으면 false
	inline bool RayBox(const FVector3& LocalOrigin, const FVector3& LocalDirection, const FVector3& HalfExtents, float& OutNear, float& OutFar)
	{
		float Near = -1.0e30f;
		float Far  = 1.0e30f;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float O = LocalOrigin[Axis];
			const float D = LocalDirection[Axis];
			if (FMath::Abs(D) < 1.0e-8f)
			{
				if (O < -HalfExtents[Axis] || O > HalfExtents[Axis])
				{
					return false;
				}
				continue;
			}
			float T0 = (-HalfExtents[Axis] - O) / D;
			float T1 = (HalfExtents[Axis] - O) / D;
			if (T0 > T1)
			{
				const float Swap = T0;
				T0               = T1;
				T1               = Swap;
			}
			Near = FMath::Max(Near, T0);
			Far  = FMath::Min(Far, T1);
		}
		OutNear = FMath::Max(Near, 0.0f);
		OutFar  = Far;
		return Far > OutNear;
	}

	// 물속을 지나는 시선 길이 (카메라 → 장면 깊이 SceneDistance까지 중 상자 안 구간)
	inline float UnderwaterPathLength(const FVector3& LocalOrigin, const FVector3& LocalDirection, const FVector3& HalfExtents, float SceneDistance)
	{
		float Near = 0.0f;
		float Far  = 0.0f;
		if (!RayBox(LocalOrigin, LocalDirection, HalfExtents, Near, Far))
		{
			return 0.0f;
		}
		return FMath::Max(FMath::Min(Far, SceneDistance) - Near, 0.0f);
	}
} // namespace FWaterMath

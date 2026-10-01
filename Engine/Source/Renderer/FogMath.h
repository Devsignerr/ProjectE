#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <cmath>

// 안개 CPU 측 순수 계산 (Engine/Shaders/Fog.hlsli, VolumetricFog.hlsl과 같은 식, 테스트 FogTests). 단위 cm
//   높이 지수 안개: 밀도 σ(z) = Density · exp(-Falloff · (z - BaseHeight)). 광선 [Start, Distance] 적분 = 광학 깊이 → 투과율 exp(-τ)
//   볼류메트릭: 프러스텀 정렬 3D 격자(화면 1/8 × 64조각), 조각 깊이 = VolumetricDistance · w² (가까울수록 촘촘), 앞→뒤 적분
struct FFogMath
{
	static constexpr uint32 VolumeTileSize   = 8;  // 화면 픽셀 / 격자 칸
	static constexpr uint32 VolumeSliceCount = 64;

	// 높이 안개 광학 깊이: 카메라 높이 OriginZ에서 방향 성분 DirZ(정규화 방향의 z)로 [Start, End] 구간 (End > Start)
	static float ComputeHeightFogOpticalDepth(float Density, float Falloff, float BaseHeight, float OriginZ, float DirZ, float Start, float End)
	{
		if (End <= Start || Density <= 0.0f)
		{
			return 0.0f;
		}
		const float Length    = End - Start;
		const float StartZ    = OriginZ + DirZ * Start;
		const float AtStart   = Density * std::exp(-Falloff * (StartZ - BaseHeight));
		const float Exponent  = Falloff * DirZ * Length;
		// ∫0^L σ(StartZ + DirZ t) dt = σ(StartZ) · (1 - e^{-f·DirZ·L}) / (f·DirZ), DirZ → 0이면 σ(StartZ)·L (테일러 1차 보정)
		// = σ(StartZ) · L · (1 - e^{-x}) / x (x = f·DirZ·L). 나누는 값이 0이 되지 않게 따로 둔다 (최적화 빌드 C4723)
		if (FMath::Abs(Exponent) < 1.0e-4f)
		{
			return AtStart * Length * (1.0f - 0.5f * Exponent);
		}
		const float Denominator = Exponent > 0.0f ? FMath::Max(Exponent, 1.0e-4f) : FMath::Min(Exponent, -1.0e-4f); // 여기서는 항상 |x| >= 1e-4
		return AtStart * Length * (1.0f - std::exp(-Denominator)) / Denominator;
	}

	// 투과율 → 안개 양 (MaxOpacity로 제한)
	static float ComputeFogAmount(float OpticalDepth, float MaxOpacity)
	{
		return FMath::Min(1.0f - std::exp(-FMath::Max(OpticalDepth, 0.0f)), FMath::Clamp(MaxOpacity, 0.0f, 1.0f));
	}

	// 방향광 산란 비중: 태양 쪽을 볼수록 1 (pow(cos, 지수)). ViewDir = 카메라 → 점, LightDir = 빛 진행 방향
	static float ComputeDirectionalInscattering(const FVector3& ViewDir, const FVector3& LightDir, float Exponent)
	{
		return std::pow(FMath::Clamp(-FVector3::Dot(ViewDir, LightDir), 0.0f, 1.0f), FMath::Max(Exponent, 1.0f));
	}

	// 헤니-그린스타인 위상 함수 (구 적분 = 1). CosTheta = 빛 진행 방향 · 카메라로 나가는 방향
	static float HenyeyGreenstein(float G, float CosTheta)
	{
		const float G2    = G * G;
		const float Denom = FMath::Max(1.0f + G2 - 2.0f * G * CosTheta, 1.0e-4f);
		return (1.0f - G2) / (4.0f * FMath::Pi * Denom * std::sqrt(Denom));
	}

	// 볼륨 조각 좌표 w ∈ [0, 1] ↔ 뷰 깊이
	static float SliceToDepth(float W, float VolumetricDistance) { return VolumetricDistance * W * W; }
	static float DepthToSlice(float Depth, float VolumetricDistance)
	{
		return std::sqrt(FMath::Clamp(Depth / FMath::Max(VolumetricDistance, 1.0f), 0.0f, 1.0f));
	}

	// 볼륨 격자 크기 (화면 픽셀 / 8, 올림)
	static uint32 GetVolumeDimension(uint32 ScreenDimension) { return FMath::Max(1u, (ScreenDimension + VolumeTileSize - 1) / VolumeTileSize); }

	// 조각 하나 적분 (Hillaire 2015 에너지 보존): 산란 S·(1 - T)/σ, 투과율 T = exp(-σ·L)
	struct FIntegration
	{
		FVector3 Scattering  = FVector3::ZeroVector;
		float    Transmittance = 1.0f;
	};
	static FIntegration IntegrateSlice(const FIntegration& Accumulated, const FVector3& InScattering, float Extinction, float Length)
	{
		const float    SliceTransmittance = std::exp(-FMath::Max(Extinction, 0.0f) * Length);
		const FVector3 Integrated = Extinction > 1.0e-7f ? (InScattering - InScattering * SliceTransmittance) / Extinction : InScattering * Length;
		FIntegration   Result;
		Result.Scattering    = Accumulated.Scattering + Integrated * Accumulated.Transmittance;
		Result.Transmittance = Accumulated.Transmittance * SliceTransmittance;
		return Result;
	}
};

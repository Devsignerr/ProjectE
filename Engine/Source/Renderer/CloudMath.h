#pragma once

#include "Core/Math/Math.h"

#include <cmath>

// 볼류메트릭 구름 (Phase 49) — CPU 참조 식. 셰이더 VolumetricClouds.hlsl과 같은 식 (테스트 CloudTests)
//   층: 바닥 고도 ~ 꼭대기 고도 (km, 대기 좌표 = 지면 기준). 높이 비율 h = (고도 - 바닥) / 두께
//   밀도 = Remap(모양 × 높이 기울기, 1 - 덮임, 1, 0, 1) × 덮임 → 세부 노이즈로 가장자리 깎기 → × 밀도 배율
//   조명: 비어-파우더(Beer-Powder) + 이중 로브 헤니-그린스타인 + 다중 산란 근사(옥타브, Wrenninge), 앰비언트 = 하늘 조도 (높이로 보간)
namespace FCloudMath
{
	inline float Remap(float Value, float OldMin, float OldMax, float NewMin, float NewMax)
	{
		return NewMin + (Value - OldMin) / FMath::Max(OldMax - OldMin, 1.0e-6f) * (NewMax - NewMin);
	}
	inline float Saturate(float Value) { return FMath::Clamp(Value, 0.0f, 1.0f); }

	// 높이 비율 h(0 = 바닥, 1 = 꼭대기). 구름 종류 Type 0 = 낮고 평평(층운), 1 = 높이 솟음(적운)
	inline float HeightGradient(float H, float Type)
	{
		const float Bottom   = Saturate(Remap(H, 0.0f, 0.1f, 0.0f, 1.0f));
		const float TopLimit = 0.3f + 0.7f * Saturate(Type);
		const float Top      = Saturate(Remap(H, TopLimit * 0.6f, TopLimit, 1.0f, 0.0f));
		return Bottom * Top;
	}

	// 기본 모양 밀도: 모양 노이즈(펄린-워리 R + 워리 FBM) × 높이 기울기 → 덮임 잘라내기
	inline float BaseDensity(float PerlinWorley, float WorleyFbm, float Gradient, float Coverage)
	{
		const float Shape   = Saturate(Remap(PerlinWorley, WorleyFbm - 1.0f, 1.0f, 0.0f, 1.0f)) * Gradient;
		const float Covered = Saturate(Remap(Shape, 1.0f - Saturate(Coverage), 1.0f, 0.0f, 1.0f));
		return Covered * Saturate(Coverage);
	}

	// 세부 노이즈로 가장자리 깎기 (밀도가 낮은 곳일수록 많이). 높이가 낮을수록 세부를 뒤집어 아래쪽을 부풀린다
	inline float ErodeDensity(float Base, float DetailFbm, float H, float Strength)
	{
		const float Detail = FMath::Lerp(DetailFbm, 1.0f - DetailFbm, Saturate(H * 4.0f));
		return Saturate(Remap(Base, Detail * Strength, 1.0f, 0.0f, 1.0f));
	}

	inline float HenyeyGreenstein(float CosTheta, float G)
	{
		const float G2 = G * G;
		return (1.0f - G2) / (4.0f * FMath::Pi * std::pow(FMath::Max(1.0f + G2 - 2.0f * G * CosTheta, 1.0e-6f), 1.5f));
	}
	// 앞쪽(은빛 가장자리) + 뒤쪽 로브
	inline float DualLobePhase(float CosTheta, float ForwardG, float BackwardG, float Blend)
	{
		return FMath::Lerp(HenyeyGreenstein(CosTheta, ForwardG), HenyeyGreenstein(CosTheta, BackwardG), Blend);
	}
	// 비어-파우더: 광학 깊이 → 빛 투과 (가장자리 어두운 효과는 파우더 항)
	inline float BeerPowder(float OpticalDepth, float PowderStrength)
	{
		return std::exp(-OpticalDepth) * FMath::Lerp(1.0f, 1.0f - std::exp(-2.0f * OpticalDepth), Saturate(PowderStrength));
	}
	// 다중 산란 근사 (Wrenninge 2013): 옥타브 i마다 소멸 a^i, 산란 b^i, 위상 이심률 c^i
	inline float MultiScatteringOctaves(float OpticalDepthToLight, float CosTheta, float ForwardG, float BackwardG, float LobeBlend, uint32 Octaves)
	{
		float Sum = 0.0f;
		float A   = 1.0f;
		float B   = 1.0f;
		float C   = 1.0f;
		for (uint32 Index = 0; Index < Octaves; ++Index)
		{
			Sum += B * std::exp(-OpticalDepthToLight * A) * DualLobePhase(CosTheta, ForwardG * C, BackwardG * C, LobeBlend);
			A *= 0.5f;
			B *= 0.5f;
			C *= 0.5f;
		}
		return Sum;
	}

	// 광선 - 구 껍질 [Inner, Outer] 구간 (원점 = 행성 중심, 카메라 반지름 CameraRadius, 천정 코사인 Mu). 없으면 false
	// 카메라가 껍질 아래에 있으면 [안쪽 구 출구, 바깥 구 출구] (지면에 막히는 아래 방향은 없음)
	inline bool RayShellIntersection(float CameraRadius, float Mu, float InnerRadius, float OuterRadius, float GroundRadius, float& OutStart, float& OutEnd)
	{
		const auto Intersect = [](float R, float M, float SphereRadius, float& T0, float& T1) {
			const float B = R * M;
			const float C = R * R - SphereRadius * SphereRadius;
			const float D = B * B - C;
			if (D < 0.0f)
			{
				return false;
			}
			const float Root = std::sqrt(D);
			T0               = -B - Root;
			T1               = -B + Root;
			return true;
		};
		float OuterT0 = 0.0f;
		float OuterT1 = 0.0f;
		if (!Intersect(CameraRadius, Mu, OuterRadius, OuterT0, OuterT1) || OuterT1 <= 0.0f)
		{
			return false;
		}
		float InnerT0 = 0.0f;
		float InnerT1 = 0.0f;
		const bool bInner = Intersect(CameraRadius, Mu, InnerRadius, InnerT0, InnerT1);
		if (CameraRadius < InnerRadius)
		{
			// 껍질 아래: 지면에 막히면 없음
			float GroundT0 = 0.0f;
			float GroundT1 = 0.0f;
			if (Intersect(CameraRadius, Mu, GroundRadius, GroundT0, GroundT1) && GroundT0 > 0.0f)
			{
				return false;
			}
			OutStart = bInner ? FMath::Max(InnerT1, 0.0f) : 0.0f;
			OutEnd   = OuterT1;
		}
		else if (CameraRadius <= OuterRadius)
		{
			// 껍질 안: 아래로 안쪽 구를 만나면 거기까지
			OutStart = 0.0f;
			OutEnd   = (bInner && InnerT0 > 0.0f) ? InnerT0 : OuterT1;
		}
		else
		{
			// 껍질 위
			OutStart = FMath::Max(OuterT0, 0.0f);
			OutEnd   = (bInner && InnerT0 > 0.0f) ? InnerT0 : OuterT1;
		}
		return OutEnd > OutStart;
	}
} // namespace FCloudMath

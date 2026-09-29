#pragma once

#include "Core/Math/Math.h"

#include <cmath>

// GPU 적분의 CPU 참조 식. Ibl.hlsl과 함께 유지한다.
namespace IblMath
{
	constexpr uint32 SkyCubeSize = 128;
	constexpr uint32 IrradianceSize = 32;
	constexpr uint32 PrefilterSize = 128;
	constexpr uint32 PrefilterMipCount = 6;
	constexpr uint32 BrdfLutSize = 128;
	constexpr uint32 IntegrationSampleCount = 256;

	// D3D 큐브 면: +X, -X, +Y, -Y, +Z, -Z.
	// U와 V는 [-1, 1], V는 아래쪽이 양수.
	inline FVector3 CubeFaceUVToDirection(uint32 Face, float U, float V)
	{
		FVector3 Direction;
		switch (Face)
		{
		case 0: Direction = FVector3(1.0f, -V, -U); break;
		case 1: Direction = FVector3(-1.0f, -V, U); break;
		case 2: Direction = FVector3(U, 1.0f, V); break;
		case 3: Direction = FVector3(U, -1.0f, -V); break;
		case 4: Direction = FVector3(U, -V, 1.0f); break;
		default: Direction = FVector3(-U, -V, -1.0f); break;
		}
		return Direction.GetNormalized();
	}

	inline void DirectionToCubeFaceUV(
		const FVector3& Direction, uint32& OutFace, float& OutU, float& OutV)
	{
		const float AX = FMath::Abs(Direction.X);
		const float AY = FMath::Abs(Direction.Y);
		const float AZ = FMath::Abs(Direction.Z);

		if (AX == 0.0f && AY == 0.0f && AZ == 0.0f)
		{
			OutFace = 0;
			OutU = 0.0f;
			OutV = 0.0f;
			return;
		}

		if (AX >= AY && AX >= AZ)
		{
			OutFace = Direction.X > 0.0f ? 0u : 1u;
			OutU = (Direction.X > 0.0f ? -Direction.Z : Direction.Z) / AX;
			OutV = -Direction.Y / AX;
		}
		else if (AY >= AZ)
		{
			OutFace = Direction.Y > 0.0f ? 2u : 3u;
			OutU = Direction.X / AY;
			OutV = (Direction.Y > 0.0f ? Direction.Z : -Direction.Z) / AY;
		}
		else
		{
			OutFace = Direction.Z > 0.0f ? 4u : 5u;
			OutU = (Direction.Z > 0.0f ? Direction.X : -Direction.X) / AZ;
			OutV = -Direction.Y / AZ;
		}
	}

	inline float MipToRoughness(uint32 Mip, uint32 MipCount)
	{
		return MipCount <= 1
			? 0.0f
			: static_cast<float>(FMath::Min(Mip, MipCount - 1))
			  / static_cast<float>(MipCount - 1);
	}

	inline float RadicalInverseVdC(uint32 Bits)
	{
		Bits = (Bits << 16u) | (Bits >> 16u);
		Bits = ((Bits & 0x55555555u) << 1u) | ((Bits & 0xAAAAAAAAu) >> 1u);
		Bits = ((Bits & 0x33333333u) << 2u) | ((Bits & 0xCCCCCCCCu) >> 2u);
		Bits = ((Bits & 0x0F0F0F0Fu) << 4u) | ((Bits & 0xF0F0F0F0u) >> 4u);
		Bits = ((Bits & 0x00FF00FFu) << 8u) | ((Bits & 0xFF00FF00u) >> 8u);
		return static_cast<float>(Bits) * 2.3283064365386963e-10f;
	}

	inline FVector2 Hammersley(uint32 Index, uint32 Count)
	{
		return FVector2(
			Count == 0 ? 0.0f : static_cast<float>(Index) / static_cast<float>(Count),
			RadicalInverseVdC(Index));
	}

	// 로컬 법선 +Z 기준 GGX 중요도 샘플링.
	inline FVector3 ImportanceSampleGGXTangent(const FVector2& Xi, float Roughness)
	{
		const float Alpha = Roughness * Roughness;
		const float Phi = 2.0f * FMath::Pi * Xi.X;
		const float CosTheta = FMath::Sqrt(
			(1.0f - Xi.Y)
			/ FMath::Max(1.0f + (Alpha * Alpha - 1.0f) * Xi.Y, 1.0e-7f));
		const float SinTheta = FMath::Sqrt(FMath::Max(0.0f, 1.0f - CosTheta * CosTheta));

		return FVector3(
			SinTheta * FMath::Cos(Phi),
			SinTheta * FMath::Sin(Phi),
			CosTheta);
	}

	inline float GeometrySmithIbl(float NdotV, float NdotL, float Roughness)
	{
		const float K = Roughness * Roughness / 2.0f;
		const float GV = NdotV / FMath::Max(NdotV * (1.0f - K) + K, 1.0e-6f);
		const float GL = NdotL / FMath::Max(NdotL * (1.0f - K) + K, 1.0e-6f);
		return GV * GL;
	}

	// 분할 합 BRDF: Specular = Prefiltered * (F0 * A + B).
	inline FVector2 IntegrateBrdf(float NdotV, float Roughness, uint32 SampleCount)
	{
		if (SampleCount == 0)
		{
			return FVector2(0.0f, 0.0f);
		}

		NdotV = FMath::Clamp(NdotV, 1.0e-4f, 1.0f);
		Roughness = FMath::Clamp(Roughness, 0.0f, 1.0f);

		const FVector3 V(
			FMath::Sqrt(FMath::Max(0.0f, 1.0f - NdotV * NdotV)), 0.0f, NdotV);
		float A = 0.0f;
		float B = 0.0f;

		for (uint32 Index = 0; Index < SampleCount; ++Index)
		{
			const FVector3 H = ImportanceSampleGGXTangent(
				Hammersley(Index, SampleCount), Roughness);
			const float RawVdotH = FVector3::Dot(V, H);
			const FVector3 L = H * (2.0f * RawVdotH) - V;
			const float NdotL = FMath::Clamp(L.Z, 0.0f, 1.0f);

			if (NdotL <= 0.0f)
			{
				continue;
			}

			const float NdotH = FMath::Clamp(H.Z, 0.0f, 1.0f);
			const float VdotH = FMath::Clamp(RawVdotH, 0.0f, 1.0f);
			const float GVis = GeometrySmithIbl(NdotV, NdotL, Roughness)
				* VdotH / FMath::Max(NdotH * NdotV, 1.0e-6f);
			const float Fc = std::pow(1.0f - VdotH, 5.0f);

			A += (1.0f - Fc) * GVis;
			B += Fc * GVis;
		}

		return FVector2(
			A / static_cast<float>(SampleCount),
			B / static_cast<float>(SampleCount));
	}
}

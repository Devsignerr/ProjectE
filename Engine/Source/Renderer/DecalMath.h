#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

// 박스 투영 데칼 CPU 측 순수 계산 (Engine/Shaders/Decal.hlsl, Mesh.hlsl DBuffer 적용과 같은 식, 테스트 DecalTests)
//   데칼 상자 = 엔티티 로컬 [-Size/2, Size/2]. 로컬 -Z 방향으로 찍는다 (표면 쪽 법선 = 로컬 +Z, 기본 회전이면 바닥).
//   UV: U = 로컬 +Y(오른쪽), V = 로컬 -X 방향 (위에서 +X를 보면 텍스처가 바로 선다). 노멀 맵 탄젠트 = +Y, 텍스처 위 = +X
//   DBuffer(A 베이스색, B 법선, C 거칠기/금속): 데칼마다 rgb = 값·불투명도 누적, a = 남은 원래 표면 비중 (지움 = (0, 0, 0, 1))
//     → 표면 값 = 원래 값 × a + rgb. 법선은 (n*0.5+0.5)을 누적하므로 Σn·w = 2·rgb - (1 - a)
struct FDecalMath
{
	// 단위 상자([-0.5, 0.5]^3) → 월드
	static FMatrix4x4 MakeDecalToWorld(const FMatrix4x4& EntityWorld, const FVector3& Size)
	{
		return FMatrix4x4::MakeScale(FVector3(FMath::Max(Size.X, 0.01f), FMath::Max(Size.Y, 0.01f), FMath::Max(Size.Z, 0.01f))) * EntityWorld;
	}

	// 단위 상자 로컬 위치 → UV
	static FVector2 LocalToUv(const FVector3& Local) { return FVector2(Local.Y + 0.5f, 0.5f - Local.X); }

	static bool IsInsideBox(const FVector3& Local)
	{
		return FMath::Abs(Local.X) <= 0.5f && FMath::Abs(Local.Y) <= 0.5f && FMath::Abs(Local.Z) <= 0.5f;
	}

	// 각도 페이드: 표면 법선과 투영 축(표면 쪽 법선)의 내적. 0.25 이하 0 → 0.5 이상 1 (옆면 늘어짐/뒷면 방지)
	static float ComputeAngleFade(float NormalDotAxis) { return FMath::Clamp((NormalDotAxis - 0.25f) / 0.25f, 0.0f, 1.0f); }

	// 깊이(투영 축) 페이드: 상자 위/아래 끝 10%에서 사라진다
	static float ComputeDepthFade(float LocalZ) { return FMath::Clamp((0.5f - FMath::Abs(LocalZ)) / 0.1f, 0.0f, 1.0f); }

	// 카메라 거리 페이드: Start 이전 1 → End에서 0. End <= Start면 페이드 없음
	static float ComputeDistanceFade(float Distance, float Start, float End)
	{
		if (End <= Start)
		{
			return 1.0f;
		}
		return FMath::Clamp((End - Distance) / (End - Start), 0.0f, 1.0f);
	}

	// DBuffer 블렌드 한 번 (rgb = Src·a + Dst·(1-a), 남은 비중 = Dst·(1-a))
	struct FDBufferValue
	{
		FVector3 Accumulated = FVector3::ZeroVector;
		float    Remaining   = 1.0f;
	};
	static FDBufferValue Blend(const FDBufferValue& Dest, const FVector3& Value, float Alpha)
	{
		FDBufferValue Result;
		Result.Accumulated = Value * Alpha + Dest.Accumulated * (1.0f - Alpha);
		Result.Remaining   = Dest.Remaining * (1.0f - Alpha);
		return Result;
	}

	// 메인 패스 적용
	static FVector3 Apply(const FVector3& Surface, const FDBufferValue& Stored) { return Surface * Stored.Remaining + Stored.Accumulated; }
	static FVector3 ApplyNormal(const FVector3& SurfaceNormal, const FDBufferValue& Stored)
	{
		return (SurfaceNormal * Stored.Remaining + Stored.Accumulated * 2.0f - FVector3(1.0f - Stored.Remaining)).GetNormalized();
	}
};

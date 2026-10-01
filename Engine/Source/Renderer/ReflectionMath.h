#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Renderer/IblMath.h"

#include <cmath>

// 반사 CPU 측 순수 계산 (SsrTrace.hlsl / Mesh.hlsl 반사 캡처, 테스트 ReflectionTests)
//   우선순위: SSR(신뢰도 a) → 반사 캡처(우선순위 내림차순으로 남은 비중을 채움) → 하늘 IBL(남은 비중)
//   캡처 영향 = 구(반경) 또는 상자(반 크기, 월드 축 정렬 — 엔티티 회전 무시), 경계 안쪽 FadeDistance에서 0 → 1.
//   상자 캡처는 시차 보정: 반사 광선과 상자의 교점 - 캡처 위치 방향으로 큐브를 읽는다
struct FReflectionMath
{
	static constexpr uint32 CaptureSize      = 128;
	static constexpr uint32 CaptureMipCount  = IblMath::PrefilterMipCount; // 하늘 프리필터와 같은 거칠기 → 밉 대응
	static constexpr uint32 MaxCaptures      = 8;

	// 큐브 면 f를 그릴 카메라 축 (D3D 면 순서 +X,-X,+Y,-Y,+Z,-Z, IblMath::CubeFaceUVToDirection과 같은 방향 규약):
	//   앞 = 면 중심 방향, 오른쪽 = +U 방향, 위 = -V 방향. 왼손 Z-up에서 Cross(앞, 오른쪽) = 위가 성립한다
	static void GetCubeFaceBasis(uint32 Face, FVector3& OutForward, FVector3& OutRight, FVector3& OutUp)
	{
		OutForward = IblMath::CubeFaceUVToDirection(Face, 0.0f, 0.0f);
		OutRight   = (IblMath::CubeFaceUVToDirection(Face, 1.0f, 0.0f) * FMath::Sqrt(2.0f) - OutForward).GetNormalized();
		OutUp      = (OutForward - IblMath::CubeFaceUVToDirection(Face, 0.0f, 1.0f) * FMath::Sqrt(2.0f)).GetNormalized();
	}

	static FQuat MakeBasisRotation(const FVector3& Forward, const FVector3& Right, const FVector3& Up)
	{
		const float M[4][4] = { { Forward.X, Forward.Y, Forward.Z, 0.0f }, { Right.X, Right.Y, Right.Z, 0.0f }, { Up.X, Up.Y, Up.Z, 0.0f },
		                        { 0.0f, 0.0f, 0.0f, 1.0f } };
		return FQuat::FromRotationMatrix(M);
	}

	// 캡처 영향 비중 [0, 1] (Shape 0 = 구, 1 = 상자)
	static float ComputeInfluence(uint32 Shape, const FVector3& Point, const FVector3& Center, float Radius, const FVector3& Extent, float FadeDistance)
	{
		const float Fade = FMath::Max(FadeDistance, 1.0e-3f);
		if (Shape == 0)
		{
			const float Distance = FVector3::Distance(Point, Center);
			return FMath::Clamp((Radius - Distance) / Fade, 0.0f, 1.0f);
		}
		const FVector3 Local = Point - Center;
		const float    Inside = FMath::Min(Extent.X - FMath::Abs(Local.X), FMath::Min(Extent.Y - FMath::Abs(Local.Y), Extent.Z - FMath::Abs(Local.Z)));
		return FMath::Clamp(Inside / Fade, 0.0f, 1.0f);
	}

	// 상자 시차 보정: 상자 안 점 Point에서 방향 Dir로 나가 만나는 상자 면 → 캡처 중심 기준 방향
	static FVector3 ParallaxCorrect(const FVector3& Point, const FVector3& Dir, const FVector3& Center, const FVector3& Extent)
	{
		const FVector3 BoxMin = Center - Extent;
		const FVector3 BoxMax = Center + Extent;
		float          TExit  = 1.0e30f;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float D = Dir[Axis];
			if (FMath::Abs(D) > 1.0e-6f)
			{
				const float Plane = D > 0.0f ? BoxMax[Axis] : BoxMin[Axis];
				TExit             = FMath::Min(TExit, (Plane - Point[Axis]) / D);
			}
		}
		const FVector3 Hit = Point + Dir * FMath::Max(TExit, 0.0f);
		return (Hit - Center).GetNormalized();
	}

	// SSR 거칠기 페이드: Start 이하 1 → Max에서 0
	static float ComputeSsrRoughnessFade(float Roughness, float MaxRoughness)
	{
		const float Start = MaxRoughness * 0.5f;
		return FMath::Clamp((MaxRoughness - Roughness) / FMath::Max(MaxRoughness - Start, 1.0e-3f), 0.0f, 1.0f);
	}

	// SSR 거칠기 흐림 (SsrTrace.hlsl과 같은 식): 거울 방향으로 한 번 추적한 반사를 GGX 반사 로브의 원뿔 크기만큼 화면에서 흐린다.
	//   원뿔 반각 = acos(0.244^(1/(p+1))), p = 2/alpha^2 - 2 (alpha = 거칠기^2, 퐁 지수 근사). 반환은 tan(반각)
	static float ComputeSpecularConeTangent(float Roughness)
	{
		const float Alpha = Roughness * Roughness;
		if (Alpha < 1.0e-3f)
		{
			return 0.0f;
		}
		const float Power    = FMath::Max(2.0f / (Alpha * Alpha) - 2.0f, 0.0f);
		const float CosAngle = std::pow(0.244f, 1.0f / (Power + 1.0f));
		return std::sqrt(FMath::Max(1.0f - CosAngle * CosAngle, 0.0f)) / FMath::Max(CosAngle, 1.0e-4f);
	}

	// 반사된 상의 뷰 깊이 (원근): 거울에 비친 점은 시선 방향으로 표면 뒤 교차 거리만큼 간 가상 점에 보인다.
	//   = 표면 뷰 깊이 × (1 + 교차 거리 / 카메라 → 표면 거리). 흐림 반경의 픽셀/거리는 표면이 아니라 이 깊이로 나눈다
	static float ComputeSsrReflectionViewDepth(float SurfaceViewDepth, float SurfaceDistance, float HitDistance)
	{
		return SurfaceViewDepth * (1.0f + HitDistance / FMath::Max(SurfaceDistance, 1.0e-3f));
	}

	// 화면 흐림 반경(픽셀) = 교차 거리 × tan(반각) × 픽셀/거리
	//   (원근: 투영[1][1] × 높이/2 ÷ 반사된 상의 뷰 깊이 ComputeSsrReflectionViewDepth, 직교: 투영[1][1] × 높이/2)
	static float ComputeSsrBlurRadiusPixels(float Roughness, float HitDistance, float PixelsPerUnit, float MaxRadius)
	{
		return FMath::Min(HitDistance * ComputeSpecularConeTangent(Roughness) * PixelsPerUnit, MaxRadius);
	}

	// Hi-Z 밉 수 (한 변이 1이 될 때까지, 최대 MaxMips)
	static uint32 GetHizMipCount(uint32 Width, uint32 Height, uint32 MaxMips = 10)
	{
		uint32 Count = 1;
		uint32 Size  = FMath::Max(Width, Height);
		while (Size > 1 && Count < MaxMips)
		{
			Size >>= 1;
			++Count;
		}
		return Count;
	}
};

#pragma once

#include "Core/Math/Math.h"

#include <array>

// 캐스케이드 섀도우 맵 계산 (GPU 없이 테스트 가능한 순수 함수)
namespace ShadowMath
{
	constexpr uint32 MaxCascades = 4;

	// 실용 분할(Practical Split Scheme): 로그 분할과 균등 분할을 Lambda로 섞는다.
	// 반환: 각 캐스케이드의 뷰 공간 far 거리 (Count개, 마지막 = FarZ)
	inline std::array<float, MaxCascades> ComputeCascadeSplits(float NearZ, float FarZ, uint32 Count, float Lambda)
	{
		std::array<float, MaxCascades> Splits{};
		Count = FMath::Clamp<uint32>(Count, 1, MaxCascades);
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			const float P       = static_cast<float>(Index + 1) / static_cast<float>(Count);
			const float LogPart = NearZ * std::pow(FarZ / NearZ, P);
			const float UniPart = NearZ + (FarZ - NearZ) * P;
			Splits[Index]       = FMath::Lerp(UniPart, LogPart, Lambda);
		}
		Splits[Count - 1] = FarZ;
		return Splits;
	}

	// 원근 카메라 절두체의 [SliceNear, SliceFar] 구간 8개 꼭짓점 (월드)
	inline std::array<FVector3, 8> ComputeFrustumSliceCorners(const FVector3& Position, const FVector3& Forward, const FVector3& Right,
	                                                         const FVector3& Up, float FovYRadians, float Aspect, float SliceNear,
	                                                         float SliceFar)
	{
		std::array<FVector3, 8> Corners{};
		const float             TanY = FMath::Tan(FovYRadians * 0.5f);
		const float             TanX = TanY * Aspect;
		const float             Depths[2] = { SliceNear, SliceFar };
		int32                   Out       = 0;
		for (float Depth : Depths)
		{
			const FVector3 Center = Position + Forward * Depth;
			const FVector3 HalfX  = Right * (TanX * Depth);
			const FVector3 HalfY  = Up * (TanY * Depth);
			Corners[Out++]        = Center - HalfX - HalfY;
			Corners[Out++]        = Center + HalfX - HalfY;
			Corners[Out++]        = Center - HalfX + HalfY;
			Corners[Out++]        = Center + HalfX + HalfY;
		}
		return Corners;
	}

	// 직교 카메라 상자의 [SliceNear, SliceFar] 구간 8개 꼭짓점 (월드). HalfWidth/HalfHeight = 화면 절반 크기(cm)
	inline std::array<FVector3, 8> ComputeOrthoSliceCorners(const FVector3& Position, const FVector3& Forward, const FVector3& Right,
	                                                       const FVector3& Up, float HalfWidth, float HalfHeight, float SliceNear,
	                                                       float SliceFar)
	{
		std::array<FVector3, 8> Corners{};
		const float             Depths[2] = { SliceNear, SliceFar };
		int32                   Out       = 0;
		for (float Depth : Depths)
		{
			const FVector3 Center = Position + Forward * Depth;
			const FVector3 HalfX  = Right * HalfWidth;
			const FVector3 HalfY  = Up * HalfHeight;
			Corners[Out++]        = Center - HalfX - HalfY;
			Corners[Out++]        = Center + HalfX - HalfY;
			Corners[Out++]        = Center - HalfX + HalfY;
			Corners[Out++]        = Center + HalfX + HalfY;
		}
		return Corners;
	}

	struct FCascade
	{
		FMatrix4x4 ViewProjection; // 월드 → 섀도우 클립 (행벡터)
		float      WorldTexelSize = 0.0f; // 섀도우 맵 텍셀 하나의 월드 크기 (노멀 오프셋용)
		FVector3   SphereCenter;
		float      SphereRadius = 0.0f;
	};

	// 절두체 조각을 감싸는 구 → 광원 방향 직교 투영. 텍셀 격자에 스냅해 카메라 이동 시 그림자 가장자리 떨림을 막는다.
	//   CasterExtension: 조각 앞쪽(광원 쪽)으로 더 확장해 화면 밖 캐스터도 그림자를 드리우게 한다
	inline FCascade ComputeCascade(const std::array<FVector3, 8>& Corners, const FVector3& LightDirection, uint32 Resolution,
	                               float CasterExtension)
	{
		FCascade Cascade;

		FVector3 Center = FVector3::ZeroVector;
		for (const FVector3& Corner : Corners)
		{
			Center += Corner;
		}
		Center /= 8.0f;

		float Radius = 0.0f;
		for (const FVector3& Corner : Corners)
		{
			Radius = FMath::Max(Radius, FVector3::Distance(Center, Corner));
		}
		// 반경을 1/16 단위로 올림 → 프레임 간 투영 크기 고정
		// 중심 스냅으로 이동하는 최대 한 텍셀을 포함하도록 투영 영역을 확장한다.
		Radius *= static_cast<float>(Resolution) / static_cast<float>(Resolution - 2);
		Radius = FMath::Ceil(Radius * 16.0f) / 16.0f;

		const FVector3 Direction = LightDirection.GetNormalized();
		const FVector3 WorldUp   = FMath::Abs(FVector3::Dot(Direction, FVector3::UpVector)) > 0.99f ? FVector3::ForwardVector : FVector3::UpVector;

		// 텍셀 스냅: 원점 기준 광원 뷰에서 중심을 텍셀 격자에 맞춘다
		const float      TexelSize  = (Radius * 2.0f) / static_cast<float>(Resolution);
		const FMatrix4x4 LightView0 = FMatrix4x4::MakeLookAt(FVector3::ZeroVector, Direction, WorldUp);
		FVector3         CenterLS   = LightView0.TransformPosition(Center);
		CenterLS.X                  = FMath::Floor(CenterLS.X / TexelSize) * TexelSize;
		CenterLS.Y                  = FMath::Floor(CenterLS.Y / TexelSize) * TexelSize;
		Center                      = LightView0.GetInverse().TransformPosition(CenterLS);

		const FVector3   Eye  = Center - Direction * (Radius + CasterExtension);
		const FMatrix4x4 View = FMatrix4x4::MakeLookAt(Eye, Center, WorldUp);
		const FMatrix4x4 Proj = FMatrix4x4::MakeOrthographic(Radius * 2.0f, Radius * 2.0f, 0.0f, Radius * 2.0f + CasterExtension);

		Cascade.ViewProjection = View * Proj;
		Cascade.WorldTexelSize = TexelSize;
		Cascade.SphereCenter   = Center;
		Cascade.SphereRadius   = Radius;
		return Cascade;
	}
} // namespace ShadowMath

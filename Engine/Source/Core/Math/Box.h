#pragma once

#include "Core/Math/Matrix4x4.h"
#include "Core/Math/Vector3.h"

#include <limits>

// 축 정렬 경계 상자 (AABB). 기본 상태는 비어 있음(IsValid() == false).
struct FBox
{
	FVector3 Min = FVector3(std::numeric_limits<float>::max());
	FVector3 Max = FVector3(std::numeric_limits<float>::lowest());

	constexpr FBox() = default;
	constexpr FBox(const FVector3& InMin, const FVector3& InMax) : Min(InMin), Max(InMax) {}

	constexpr bool IsValid() const { return Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z; }

	constexpr void AddPoint(const FVector3& P)
	{
		Min = FVector3(FMath::Min(Min.X, P.X), FMath::Min(Min.Y, P.Y), FMath::Min(Min.Z, P.Z));
		Max = FVector3(FMath::Max(Max.X, P.X), FMath::Max(Max.Y, P.Y), FMath::Max(Max.Z, P.Z));
	}

	constexpr void AddBox(const FBox& Other)
	{
		if (Other.IsValid())
		{
			AddPoint(Other.Min);
			AddPoint(Other.Max);
		}
	}

	constexpr FVector3 GetCenter() const { return (Min + Max) * 0.5f; }
	constexpr FVector3 GetExtent() const { return (Max - Min) * 0.5f; } // 반 크기
	constexpr FVector3 GetSize() const { return Max - Min; }

	constexpr bool Contains(const FVector3& P) const
	{
		return P.X >= Min.X && P.X <= Max.X && P.Y >= Min.Y && P.Y <= Max.Y && P.Z >= Min.Z && P.Z <= Max.Z;
	}

	constexpr bool Intersects(const FBox& Other) const
	{
		return Min.X <= Other.Max.X && Max.X >= Other.Min.X && Min.Y <= Other.Max.Y && Max.Y >= Other.Min.Y &&
		       Min.Z <= Other.Max.Z && Max.Z >= Other.Min.Z;
	}

	// 8개 꼭짓점을 변환해 감싸는 새 AABB (회전 시 보수적으로 커진다)
	FBox TransformBy(const FMatrix4x4& Matrix) const
	{
		if (!IsValid())
		{
			return FBox();
		}

		FBox Result;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 Point((Corner & 1) ? Max.X : Min.X, (Corner & 2) ? Max.Y : Min.Y, (Corner & 4) ? Max.Z : Min.Z);
			Result.AddPoint(Matrix.TransformPosition(Point));
		}
		return Result;
	}

	bool Equals(const FBox& Other, float Tolerance = FMath::KindaSmallNumber) const
	{
		return Min.Equals(Other.Min, Tolerance) && Max.Equals(Other.Max, Tolerance);
	}
};

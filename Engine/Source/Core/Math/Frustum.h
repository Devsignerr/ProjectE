#pragma once

#include "Core/Math/Box.h"
#include "Core/Math/Matrix4x4.h"

// 평면: Dot(Normal, P) + D = 0. 양수 쪽이 안쪽(앞)
struct FPlane
{
	FVector3 Normal = FVector3::UpVector;
	float    D      = 0.0f;

	constexpr FPlane() = default;
	constexpr FPlane(const FVector3& InNormal, float InD) : Normal(InNormal), D(InD) {}
	constexpr FPlane(float A, float B, float C, float InD) : Normal(A, B, C), D(InD) {}

	constexpr float DistanceTo(const FVector3& P) const { return FVector3::Dot(Normal, P) + D; }

	FPlane GetNormalized() const
	{
		const float Length = Normal.Length();
		return Length > FMath::SmallNumber ? FPlane(Normal / Length, D / Length) : *this;
	}
};

// 뷰 프러스텀 (6평면, 안쪽이 양수)
struct FFrustum
{
	enum EPlane : int32
	{
		Left = 0,
		Right,
		Bottom,
		Top,
		Near,
		Far,
		Count
	};

	FPlane Planes[Count];

	// 행벡터 규약 ViewProjection에서 추출. 클립 공간: -w<=x<=w, -w<=y<=w, 0<=z<=w
	static FFrustum FromViewProjection(const FMatrix4x4& ViewProjection)
	{
		const auto Column = [&](int32 Col) {
			return FPlane(ViewProjection.M[0][Col], ViewProjection.M[1][Col], ViewProjection.M[2][Col], ViewProjection.M[3][Col]);
		};
		const FPlane C0 = Column(0);
		const FPlane C1 = Column(1);
		const FPlane C2 = Column(2);
		const FPlane C3 = Column(3);

		FFrustum Frustum;
		Frustum.Planes[Left]   = FPlane(C3.Normal + C0.Normal, C3.D + C0.D).GetNormalized(); // x + w >= 0
		Frustum.Planes[Right]  = FPlane(C3.Normal - C0.Normal, C3.D - C0.D).GetNormalized(); // w - x >= 0
		Frustum.Planes[Bottom] = FPlane(C3.Normal + C1.Normal, C3.D + C1.D).GetNormalized(); // y + w >= 0
		Frustum.Planes[Top]    = FPlane(C3.Normal - C1.Normal, C3.D - C1.D).GetNormalized(); // w - y >= 0
		Frustum.Planes[Near]   = FPlane(C2.Normal, C2.D).GetNormalized();                    // z >= 0
		Frustum.Planes[Far]    = FPlane(C3.Normal - C2.Normal, C3.D - C2.D).GetNormalized(); // w - z >= 0
		return Frustum;
	}

	constexpr bool Contains(const FVector3& Point) const
	{
		for (const FPlane& Plane : Planes)
		{
			if (Plane.DistanceTo(Point) < 0.0f)
			{
				return false;
			}
		}
		return true;
	}

	// AABB가 프러스텀과 겹치는지 (보수적: 모서리 근처에서 드물게 참을 반환할 수 있음)
	constexpr bool Intersects(const FBox& Box) const
	{
		if (!Box.IsValid())
		{
			return false;
		}
		for (const FPlane& Plane : Planes)
		{
			// 평면 법선 방향으로 가장 먼 꼭짓점(p-vertex)이 바깥이면 상자 전체가 바깥
			const FVector3 PositiveVertex(Plane.Normal.X >= 0.0f ? Box.Max.X : Box.Min.X,
			                              Plane.Normal.Y >= 0.0f ? Box.Max.Y : Box.Min.Y,
			                              Plane.Normal.Z >= 0.0f ? Box.Max.Z : Box.Min.Z);
			if (Plane.DistanceTo(PositiveVertex) < 0.0f)
			{
				return false;
			}
		}
		return true;
	}
};

#pragma once

#include "Core/Math/Box.h"
#include "Core/Math/Matrix4x4.h"

#include <vector>

// 반직선: Origin + Direction * t (t >= 0), Direction은 정규화
struct FRay
{
	FVector3 Origin;
	FVector3 Direction = FVector3::ForwardVector;

	constexpr FRay() = default;
	FRay(const FVector3& InOrigin, const FVector3& InDirection) : Origin(InOrigin), Direction(InDirection.GetNormalized()) {}

	FVector3 GetPoint(float T) const { return Origin + Direction * T; }

	// AABB 슬랩 테스트. 교차하면 true와 진입 거리(상자 안에서 시작하면 0)
	bool Intersects(const FBox& Box, float& OutDistance) const
	{
		if (!Box.IsValid())
		{
			return false;
		}

		float TMin = 0.0f;
		float TMax = std::numeric_limits<float>::max();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float O = Origin[Axis];
			const float D = Direction[Axis];
			if (FMath::Abs(D) < FMath::SmallNumber)
			{
				// 축과 평행: 원점이 슬랩 밖이면 교차 없음
				if (O < Box.Min[Axis] || O > Box.Max[Axis])
				{
					return false;
				}
				continue;
			}
			const float InvD = 1.0f / D;
			float       T0   = (Box.Min[Axis] - O) * InvD;
			float       T1   = (Box.Max[Axis] - O) * InvD;
			if (T0 > T1)
			{
				const float Temp = T0;
				T0 = T1;
				T1 = Temp;
			}
			TMin = FMath::Max(TMin, T0);
			TMax = FMath::Min(TMax, T1);
			if (TMin > TMax)
			{
				return false;
			}
		}

		OutDistance = TMin;
		return true;
	}

	// 삼각형 교차 (양면, Möller–Trumbore). 교차하면 true와 거리 t (>= 0)
	bool IntersectsTriangle(const FVector3& A, const FVector3& B, const FVector3& C, float& OutDistance) const
	{
		const FVector3 Edge1 = B - A;
		const FVector3 Edge2 = C - A;
		const FVector3 P     = FVector3::Cross(Direction, Edge2);
		const float    Det   = FVector3::Dot(Edge1, P);
		// 광선과 평행한 면(또는 넓이 0): 상대 판정 (cm 단위 큰 삼각형/작은 삼각형 모두)
		if (FMath::Abs(Det) <= 1.0e-7f * Edge1.Length() * Edge2.Length())
		{
			return false;
		}
		const float    InvDet = 1.0f / Det;
		const FVector3 S      = Origin - A;
		const float    U      = FVector3::Dot(S, P) * InvDet;
		if (U < 0.0f || U > 1.0f)
		{
			return false;
		}
		const FVector3 Q = FVector3::Cross(S, Edge1);
		const float    V = FVector3::Dot(Direction, Q) * InvDet;
		if (V < 0.0f || U + V > 1.0f)
		{
			return false;
		}
		const float T = FVector3::Dot(Edge2, Q) * InvDet;
		if (T < 0.0f)
		{
			return false;
		}
		OutDistance = T;
		return true;
	}

	// 삼각형 메시(로컬 정점 + 인덱스 목록, World로 배치) 중 가장 가까운 교차. MaxDistance보다 먼 교차는 무시
	bool IntersectsMesh(const FMatrix4x4& World, const std::vector<FVector3>& Positions, const std::vector<uint32>& Indices, float& OutDistance,
	                    float MaxDistance = std::numeric_limits<float>::max()) const
	{
		bool  bHit = false;
		float Best = MaxDistance;
		for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
		{
			const uint32 I0 = Indices[Index];
			const uint32 I1 = Indices[Index + 1];
			const uint32 I2 = Indices[Index + 2];
			if (I0 >= Positions.size() || I1 >= Positions.size() || I2 >= Positions.size())
			{
				continue;
			}
			float Distance = 0.0f;
			if (IntersectsTriangle(World.TransformPosition(Positions[I0]), World.TransformPosition(Positions[I1]), World.TransformPosition(Positions[I2]),
			                       Distance) &&
			    Distance < Best)
			{
				Best = Distance;
				bHit = true;
			}
		}
		if (bHit)
		{
			OutDistance = Best;
		}
		return bHit;
	}

	// 정규화 장치 좌표(NDC x,y ∈ [-1,1], y 위쪽 양수)에서 월드 반직선 생성 (뷰-투영 역행렬 사용)
	static FRay FromNdc(float NdcX, float NdcY, const FMatrix4x4& InverseViewProjection)
	{
		const FVector4 NearClip = InverseViewProjection.TransformVector4(FVector4(NdcX, NdcY, 0.0f, 1.0f));
		const FVector4 FarClip  = InverseViewProjection.TransformVector4(FVector4(NdcX, NdcY, 1.0f, 1.0f));
		const FVector3 NearWorld = NearClip.XYZ() / NearClip.W;
		const FVector3 FarWorld  = FarClip.XYZ() / FarClip.W;
		return FRay(NearWorld, FarWorld - NearWorld);
	}
};

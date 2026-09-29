#pragma once

#include "Core/Math/Box.h"
#include "Core/Math/Matrix4x4.h"

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

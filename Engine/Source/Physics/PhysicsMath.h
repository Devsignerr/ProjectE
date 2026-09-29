#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <algorithm>

// 고정 스텝 누적기: 가변 프레임 시간을 일정 간격(기본 60Hz) 스텝으로 나누고 남은 시간으로 렌더 보간 비율을 준다.
// 한 프레임 최대 스텝 수를 넘으면 초과분을 버린다 (느린 프레임 뒤 "죽음의 나선" 방지).
struct FFixedStepper
{
	float  StepSeconds = 1.0f / 60.0f;
	uint32 MaxSteps    = 4;
	float  Accumulator = 0.0f;

	// 이번 프레임에 진행할 스텝 수
	uint32 Advance(float DeltaSeconds)
	{
		Accumulator += std::max(DeltaSeconds, 0.0f);
		uint32 Steps = 0;
		while (Accumulator >= StepSeconds && Steps < MaxSteps)
		{
			Accumulator -= StepSeconds;
			++Steps;
		}
		if (Steps == MaxSteps && Accumulator >= StepSeconds)
		{
			Accumulator = 0.0f; // 따라잡지 못한 시간은 버린다 (시뮬레이션이 느려질 뿐 폭주하지 않음)
		}
		return Steps;
	}

	// 직전 스텝(0)과 다음 스텝(1) 사이 렌더 보간 비율
	float GetAlpha() const { return std::clamp(Accumulator / StepSeconds, 0.0f, 1.0f); }

	void Reset() { Accumulator = 0.0f; }
};

namespace PhysicsMath
{
	// 엔진은 cm, Jolt는 m. 축은 그대로 쓴다 (둘 다 좌표 축에 손 방향을 강제하지 않으며, 엔진 FQuat도
	// 해밀턴 곱 + v' = q v q* 규약이라 Jolt Quat와 성분이 1:1로 같다 — PhysicsTests에서 회전 결과로 검증).
	// 중력만 엔진 위(+Z) 기준으로 -Z로 준다.
	inline FVector3 ToMeters(const FVector3& Centimeters) { return Centimeters * FUnits::UnitsToMeters; }
	inline FVector3 ToCentimeters(const FVector3& Meters) { return Meters * FUnits::MetersToUnits; }

	// 월드 행렬(스케일 포함) → 위치/회전/스케일
	inline void DecomposeWorld(const FMatrix4x4& World, FVector3& OutPosition, FQuat& OutRotation, FVector3& OutScale)
	{
		World.Decompose(OutPosition, OutRotation, OutScale);
		OutRotation.Normalize();
	}

	// 월드 위치/회전 → 부모 기준 로컬 (부모 월드 행렬의 스케일은 반영, 전단은 무시)
	inline void WorldToLocal(const FMatrix4x4& ParentWorld, const FVector3& WorldPosition, const FQuat& WorldRotation,
	                         FVector3& OutLocalPosition, FQuat& OutLocalRotation)
	{
		FVector3 ParentPosition;
		FQuat    ParentRotation;
		FVector3 ParentScale;
		DecomposeWorld(ParentWorld, ParentPosition, ParentRotation, ParentScale);

		const FVector3 Unrotated = ParentRotation.UnrotateVector(WorldPosition - ParentPosition);
		OutLocalPosition         = FVector3(ParentScale.X != 0.0f ? Unrotated.X / ParentScale.X : 0.0f,
		                                    ParentScale.Y != 0.0f ? Unrotated.Y / ParentScale.Y : 0.0f,
		                                    ParentScale.Z != 0.0f ? Unrotated.Z / ParentScale.Z : 0.0f);
		OutLocalRotation         = (ParentRotation.Conjugate() * WorldRotation).GetNormalized();
	}
}

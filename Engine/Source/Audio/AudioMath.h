#pragma once

#include "Core/Math/Math.h"

// 청자(리스너) 자세. 엔진 월드 좌표(왼손 Z-up, cm)
struct FAudioListener
{
	FVector3 Position;
	FVector3 Forward = FVector3::ForwardVector;
	FVector3 Up      = FVector3::UpVector;
};

namespace AudioMath
{
	// 엔진 월드 위치 → miniaudio 청자 공간 (오른손: +X 오른쪽, +Y 위, -Z 앞, 청자는 원점).
	// 엔진과 miniaudio의 손 방향이 달라 월드 좌표를 그대로 넘기면 좌우가 뒤집히므로,
	// 청자 기준 상대 좌표로 바꿔 넘기고 miniaudio 리스너는 원점에서 -Z를 보게 고정한다.
	inline FVector3 ToListenerSpace(const FAudioListener& Listener, const FVector3& WorldPosition)
	{
		const FVector3 Forward = Listener.Forward.GetNormalized();
		// 왼손 좌표계: Cross(Forward, Right) == Up → Right = Cross(Up, Forward)
		const FVector3 Right = FVector3::Cross(Listener.Up, Forward).GetNormalized();
		const FVector3 Up    = FVector3::Cross(Forward, Right);

		const FVector3 Offset = WorldPosition - Listener.Position;
		return FVector3(FVector3::Dot(Offset, Right), FVector3::Dot(Offset, Up), -FVector3::Dot(Offset, Forward));
	}
}

#pragma once

#include "Core/CoreTypes.h"

#include <chrono>

// 프레임 단위 고해상도 타이머
class FTimer
{
public:
	FTimer();

	// 시작 시점을 현재로 재설정
	void Reset();

	// 프레임마다 한 번 호출. 델타/누적 시간 갱신
	void Tick();

	float  GetDeltaSeconds() const { return DeltaSeconds; }
	double GetTotalSeconds() const { return TotalSeconds; }
	uint64 GetFrameCount() const { return FrameCount; }

private:
	using FClock = std::chrono::steady_clock;

	FClock::time_point StartTime;
	FClock::time_point LastTime;
	float              DeltaSeconds = 0.0f;
	double             TotalSeconds = 0.0;
	uint64             FrameCount   = 0;
};

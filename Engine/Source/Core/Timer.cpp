#include "Core/Timer.h"

FTimer::FTimer()
{
	Reset();
}

void FTimer::Reset()
{
	StartTime    = FClock::now();
	LastTime     = StartTime;
	DeltaSeconds = 0.0f;
	TotalSeconds = 0.0;
	FrameCount   = 0;
}

void FTimer::Tick()
{
	const FClock::time_point Now = FClock::now();

	DeltaSeconds = std::chrono::duration<float>(Now - LastTime).count();
	TotalSeconds = std::chrono::duration<double>(Now - StartTime).count();
	LastTime     = Now;
	++FrameCount;
}

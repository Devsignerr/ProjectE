#include "Core/FrameTime.h"

namespace
{
	// 메인 스레드에서만 쓰고 읽는다
	float  GDeltaSeconds = 0.0f;
	double GTotalSeconds = 0.0;
	uint64 GFrameCount   = 0;
}

void FFrameTime::Advance(float DeltaSeconds)
{
	GDeltaSeconds = DeltaSeconds;
	GTotalSeconds += static_cast<double>(DeltaSeconds);
	++GFrameCount;
}

void FFrameTime::Reset()
{
	GDeltaSeconds = 0.0f;
	GTotalSeconds = 0.0;
	GFrameCount   = 0;
}

float FFrameTime::GetDeltaSeconds()
{
	return GDeltaSeconds;
}

double FFrameTime::GetTotalSeconds()
{
	return GTotalSeconds;
}

uint64 FFrameTime::GetFrameCount()
{
	return GFrameCount;
}

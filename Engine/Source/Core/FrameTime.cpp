#include "Core/FrameTime.h"

namespace
{
	// 메인(게임) 스레드에서만 쓴다. 다른 스레드는 FScopedOverride로 받은 값을 읽는다
	float  GDeltaSeconds = 0.0f;
	double GTotalSeconds = 0.0;
	uint64 GFrameCount   = 0;

	// 스레드별 덮어쓰기 (렌더 스레드가 기록하는 프레임의 값)
	thread_local const FFrameTime::FSnapshot* GOverride = nullptr;
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
	return GOverride != nullptr ? GOverride->DeltaSeconds : GDeltaSeconds;
}

double FFrameTime::GetTotalSeconds()
{
	return GOverride != nullptr ? GOverride->TotalSeconds : GTotalSeconds;
}

uint64 FFrameTime::GetFrameCount()
{
	return GOverride != nullptr ? GOverride->FrameCount : GFrameCount;
}

FFrameTime::FSnapshot FFrameTime::Capture()
{
	return { GetDeltaSeconds(), GetTotalSeconds(), GetFrameCount() };
}

FFrameTime::FScopedOverride::FScopedOverride(const FSnapshot& Snapshot)
	: Previous(GOverride)
	, Value(Snapshot)
{
	GOverride = &Value;
}

FFrameTime::FScopedOverride::~FScopedOverride()
{
	GOverride = Previous;
}

#include "Core/FrameTime.h"
#include "Core/Testing/TestFramework.h"

#include <thread>

// 렌더 스레드가 기록하는 프레임의 시간: 덮어쓴 스레드만 그 값을 보고, 다른 스레드(게임 스레드)는 전역 값을 본다
E_TEST(FrameTime_ScopedOverrideIsPerThread)
{
	FFrameTime::Reset();
	FFrameTime::Advance(0.25f);
	const FFrameTime::FSnapshot Captured = FFrameTime::Capture();
	E_EXPECT_EQ(Captured.FrameCount, 1u);
	E_EXPECT_NEAR(Captured.TotalSeconds, 0.25, 1e-9);

	FFrameTime::Advance(0.5f); // 게임 스레드가 다음 프레임으로
	double SeenTotal = 0.0;
	uint64 SeenCount = 0;
	float  SeenDelta = 0.0f;
	std::thread Render([&] {
		const FFrameTime::FScopedOverride Override(Captured);
		SeenTotal = FFrameTime::GetTotalSeconds();
		SeenCount = FFrameTime::GetFrameCount();
		SeenDelta = FFrameTime::GetDeltaSeconds();
	});
	Render.join();
	E_EXPECT_NEAR(SeenTotal, 0.25, 1e-9);
	E_EXPECT_EQ(SeenCount, 1u);
	E_EXPECT_NEAR(SeenDelta, 0.25f, 1e-6f);
	E_EXPECT_NEAR(FFrameTime::GetTotalSeconds(), 0.75, 1e-9); // 이 스레드는 그대로
	{
		const FFrameTime::FScopedOverride Override(Captured);
		E_EXPECT_NEAR(FFrameTime::GetTotalSeconds(), 0.25, 1e-9);
	}
	E_EXPECT_NEAR(FFrameTime::GetTotalSeconds(), 0.75, 1e-9); // 범위가 끝나면 되돌아온다
	FFrameTime::Reset();
}

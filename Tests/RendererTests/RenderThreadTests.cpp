#include "Core/FrameTime.h"
#include "Core/RenderThreadSync.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/RenderThread.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// 렌더 스레드 (Renderer/RenderThread.h): 스레드 없이/있이 같은 작업이 같은 순서로, 작업 안 FFrameTime은 넘긴 값,
// WaitIdle과 RenderThreadSync 대기 지점이 진행 중 작업을 기다린다
E_TEST(RenderThread_KickRunsInlineOrOnThreadInOrder)
{
	FRenderThread       Thread;
	std::vector<int32>  Order;
	FFrameTime::FSnapshot Time;
	Time.TotalSeconds = 12.5;
	Time.FrameCount   = 7;

	// 시작 전: 호출 스레드에서 바로
	double InlineSeen = 0.0;
	Thread.Kick([&] { Order.push_back(0); InlineSeen = FFrameTime::GetTotalSeconds(); }, Time, true);
	E_EXPECT_EQ(Order.size(), 1u);
	E_EXPECT_NEAR(InlineSeen, 12.5, 1e-9);

	Thread.Start();
	E_EXPECT_TRUE(Thread.IsRunning());
	std::atomic<bool> bOnOtherThread = false;
	double            ThreadSeen     = 0.0;
	const std::thread::id Caller     = std::this_thread::get_id();
	for (int32 Index = 1; Index <= 20; ++Index)
	{
		Thread.WaitIdle();
		Thread.Kick([&, Index] {
			Order.push_back(Index);
			bOnOtherThread = std::this_thread::get_id() != Caller;
			ThreadSeen     = FFrameTime::GetTotalSeconds();
		}, Time, true);
	}
	Thread.WaitIdle();
	E_EXPECT_FALSE(Thread.IsBusy());
	E_EXPECT_TRUE(bOnOtherThread.load());
	E_EXPECT_NEAR(ThreadSeen, 12.5, 1e-9);
	// bThreaded = false: 스레드가 있어도 바로 (r.RenderThread 0)
	Thread.Kick([&] { Order.push_back(21); }, Time, false);
	E_EXPECT_EQ(Order.size(), 22u);
	bool bInOrder = true;
	for (size_t Index = 0; Index < Order.size(); ++Index)
	{
		bInOrder = bInOrder && Order[Index] == static_cast<int32>(Index);
	}
	E_EXPECT_TRUE(bInOrder);
	const FRenderThread::FStats Stats = Thread.GetStats();
	E_EXPECT_EQ(Stats.Frames, 22u);
	E_EXPECT_EQ(Stats.Threaded, 20u);
	Thread.Stop();
	E_EXPECT_FALSE(Thread.IsRunning());
}

E_TEST(RenderThread_SyncPointWaitsForWorkInFlight)
{
	FRenderThread    Thread;
	Thread.Start();
	std::atomic<bool> bRelease  = false;
	std::atomic<bool> bFinished = false;
	Thread.Kick([&] {
		while (!bRelease.load())
		{
			std::this_thread::yield();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		bFinished = true;
	}, FFrameTime::Capture(), true);
	E_EXPECT_TRUE(Thread.IsBusy());
	bRelease = true;
	// 게임 스레드의 공유 상태 변경 지점 (리소스 관리자·콘솔 변수): 진행 중 작업이 끝난 뒤에 돌아온다
	RenderThreadSync::WaitForRenderThread();
	E_EXPECT_TRUE(bFinished.load());
	E_EXPECT_FALSE(Thread.IsBusy());

	// 렌더 스레드 안에서 부르면 기다리지 않는다 (교착 없음)
	std::atomic<bool> bInnerReturned = false;
	Thread.Kick([&] {
		RenderThreadSync::WaitForRenderThread();
		bInnerReturned = true;
	}, FFrameTime::Capture(), true);
	Thread.WaitIdle();
	E_EXPECT_TRUE(bInnerReturned.load());
	Thread.Stop();
	RenderThreadSync::WaitForRenderThread(); // 멈춘 뒤에는 처리기가 없다 — 바로 반환
}

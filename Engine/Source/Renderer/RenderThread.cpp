#include "Renderer/RenderThread.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Profiling.h"
#include "Core/RenderThreadSync.h"

#include <chrono>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	using FClock = std::chrono::steady_clock;

	// RenderThreadSync 대기 처리기가 기다릴 렌더 스레드 (동시에 하나)
	std::atomic<FRenderThread*> GActiveRenderThread{ nullptr };
	std::atomic<bool>           GWarnedRenderThreadSync{ false };

	void WaitForActiveRenderThread()
	{
		FRenderThread* const Active = GActiveRenderThread.load(std::memory_order_acquire);
		if (Active == nullptr)
		{
			return;
		}
		if (Active->IsCurrentThread())
		{
			// 렌더 스레드가 공유 상태(리소스 관리자·콘솔 변수)를 바꾸려 한다 — 규칙 위반 (RenderThread.h). 한 번만 알린다
			if (!GWarnedRenderThreadSync.exchange(true))
			{
				E_LOG(LogRenderer, Warning, "[렌더 스레드] 렌더 스레드가 게임 스레드 소유 상태를 바꿉니다 (리소스 관리자/콘솔 변수) — 게임 스레드 준비 단계로 옮겨야 합니다");
			}
			return;
		}
		Active->WaitIdle();
	}
} // namespace

FRenderThread::~FRenderThread()
{
	Stop();
}

void FRenderThread::Start()
{
	if (Thread.joinable())
	{
		return;
	}
	{
		std::lock_guard Lock(Mutex);
		bStopping = false;
		bHasWork  = false;
	}
	Thread = std::thread([this] { ThreadMain(); });
	FRenderThread* Expected = nullptr;
	GActiveRenderThread.compare_exchange_strong(Expected, this, std::memory_order_acq_rel);
	RenderThreadSync::SetWaitHandler(&WaitForActiveRenderThread);
	E_LOG(LogRenderer, Display, "[렌더 스레드] 시작 (게임 갱신과 명령 기록·제출을 겹친다)");
}

void FRenderThread::Stop()
{
	if (!Thread.joinable())
	{
		return;
	}
	WaitIdle();
	{
		std::lock_guard Lock(Mutex);
		bStopping = true;
	}
	WakeWorker.notify_all();
	Thread.join();
	ThreadId.store(std::thread::id{}, std::memory_order_release);
	FRenderThread* Expected = this;
	if (GActiveRenderThread.compare_exchange_strong(Expected, nullptr, std::memory_order_acq_rel))
	{
		RenderThreadSync::SetWaitHandler(nullptr);
	}
}

void FRenderThread::Kick(std::function<void()> Work, const FFrameTime::FSnapshot& Time, bool bThreaded)
{
	if (!bThreaded || !Thread.joinable())
	{
		WaitIdle(); // 스레드 모드에서 막 꺼졌으면 남은 작업 먼저
		RunWork(Work, Time);
		return;
	}
	{
		std::lock_guard Lock(Mutex);
		E_CHECKF(!bHasWork && !bBusy.load(std::memory_order_relaxed), "렌더 스레드 작업이 끝나기 전에 다음 작업을 넘겼습니다 (WaitIdle 먼저)");
		PendingWork = std::move(Work);
		PendingTime = Time;
		bHasWork    = true;
		bBusy.store(true, std::memory_order_release);
		++Stats.Threaded;
	}
	WakeWorker.notify_one();
}

void FRenderThread::WaitIdle()
{
	if (!bBusy.load(std::memory_order_acquire) || IsCurrentThread())
	{
		return;
	}
	E_PROFILE_SCOPE("렌더 스레드 대기");
	const FClock::time_point Start = FClock::now();
	std::unique_lock         Lock(Mutex);
	WorkDone.wait(Lock, [this] { return !bBusy.load(std::memory_order_acquire); });
	Stats.WaitMs += std::chrono::duration<double, std::milli>(FClock::now() - Start).count();
}

FRenderThread::FStats FRenderThread::GetStats() const
{
	std::lock_guard Lock(Mutex);
	return Stats;
}

void FRenderThread::ResetStats()
{
	std::lock_guard Lock(Mutex);
	Stats = FStats{};
}

void FRenderThread::RunWork(std::function<void()>& Work, const FFrameTime::FSnapshot& Time)
{
	const FClock::time_point Start = FClock::now();
	{
		const FFrameTime::FScopedOverride TimeOverride(Time);
		Work();
	}
	Work = nullptr; // 람다가 붙잡은 것(그래프·GPU 파티클 풀 등)을 이 스레드에서 놓는다
	const double Ms = std::chrono::duration<double, std::milli>(FClock::now() - Start).count();
	std::lock_guard Lock(Mutex);
	Stats.WorkMs += Ms;
	++Stats.Frames;
}

void FRenderThread::ThreadMain()
{
	ThreadId.store(std::this_thread::get_id(), std::memory_order_release);
	E_PROFILE_THREAD_NAME("렌더 스레드");
	for (;;)
	{
		std::function<void()> Work;
		FFrameTime::FSnapshot Time;
		{
			std::unique_lock Lock(Mutex);
			WakeWorker.wait(Lock, [this] { return bHasWork || bStopping; });
			if (!bHasWork)
			{
				return; // bStopping
			}
			Work     = std::move(PendingWork);
			Time     = PendingTime;
			bHasWork = false;
		}
		{
			E_PROFILE_SCOPE("렌더 스레드 프레임");
			RunWork(Work, Time);
		}
		{
			std::lock_guard Lock(Mutex);
			bBusy.store(false, std::memory_order_release);
		}
		WorkDone.notify_all();
	}
}

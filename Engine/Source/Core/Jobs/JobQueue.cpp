#include "Core/Jobs/JobQueue.h"

#include "Core/Profiling.h"

#include <algorithm>

void FJobQueue::Init(uint32 WorkerCount, const char* ThreadName)
{
	Shutdown();
	{
		std::lock_guard Lock(Mutex);
		bStopping    = false;
		bInitialized = true;
	}
	Workers.reserve(WorkerCount);
	for (uint32 Index = 0; Index < WorkerCount; ++Index)
	{
		Workers.emplace_back([this, ThreadName] {
			E_PROFILE_THREAD_NAME(ThreadName);
			WorkerMain();
		});
	}
}

void FJobQueue::Shutdown()
{
	{
		std::lock_guard Lock(Mutex);
		if (!bInitialized)
		{
			return;
		}
		bStopping = true;
		Pending.clear();
		Completed.clear();
	}
	WorkAvailable.notify_all();
	for (std::thread& Worker : Workers)
	{
		Worker.join();
	}
	Workers.clear();

	std::lock_guard Lock(Mutex);
	Completed.clear(); // 실행 중이던 작업이 남긴 완료 콜백도 버린다
	Outstanding  = 0;
	bInitialized = false;
}

void FJobQueue::Submit(FWork Work, FCompletion Completion)
{
	if (Workers.empty())
	{
		// 스레드 없음: 바로 실행 (완료는 그래도 PumpCompletions에서 — 호출 순서를 비동기와 같게)
		if (Work)
		{
			Work();
		}
		std::lock_guard Lock(Mutex);
		++Outstanding;
		Completed.push_back(std::move(Completion));
		return;
	}
	{
		std::lock_guard Lock(Mutex);
		++Outstanding;
		Pending.push_back({ std::move(Work), std::move(Completion) });
	}
	WorkAvailable.notify_one();
}

uint32 FJobQueue::PumpCompletions()
{
	std::vector<FCompletion> Ready;
	{
		std::lock_guard Lock(Mutex);
		Ready.swap(Completed);
	}
	for (FCompletion& Completion : Ready)
	{
		if (Completion)
		{
			Completion();
		}
	}
	std::lock_guard Lock(Mutex);
	Outstanding -= std::min(Outstanding, static_cast<uint32>(Ready.size()));
	return static_cast<uint32>(Ready.size());
}

void FJobQueue::WaitIdle()
{
	std::unique_lock Lock(Mutex);
	WorkDone.wait(Lock, [this] { return Pending.empty() && Running == 0; });
}

uint32 FJobQueue::GetOutstandingCount() const
{
	std::lock_guard Lock(Mutex);
	return Outstanding;
}

uint32 FJobQueue::GetDefaultWorkerCount()
{
	const uint32 Cores = std::max(1u, std::thread::hardware_concurrency());
	return std::clamp(Cores > 2 ? Cores - 2 : 1u, 1u, 4u);
}

void FJobQueue::WorkerMain()
{
	for (;;)
	{
		FJob Job;
		{
			std::unique_lock Lock(Mutex);
			WorkAvailable.wait(Lock, [this] { return bStopping || !Pending.empty(); });
			if (bStopping)
			{
				return;
			}
			Job = std::move(Pending.front());
			Pending.pop_front();
			++Running;
		}

		if (Job.Work)
		{
			E_PROFILE_SCOPE("작업");
			Job.Work();
		}

		{
			std::lock_guard Lock(Mutex);
			--Running;
			if (!bStopping)
			{
				Completed.push_back(std::move(Job.Completion));
			}
		}
		WorkDone.notify_all();
	}
}

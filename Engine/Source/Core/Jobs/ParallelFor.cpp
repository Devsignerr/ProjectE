#include "Core/Jobs/ParallelFor.h"

#include "Core/Console/Console.h"
#include "Core/Profiling.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	TAutoConsoleVariable<bool> CVarParallelFor("core.ParallelFor", true,
		"프레임 안 병렬 루프 (애니메이션·스킨 팔레트 등). 0 = 항상 호출 스레드에서 순차 실행 (비교·디버깅)");

	thread_local bool bInsideParallelFor = false;

	// 작업 하나 = 묶음 번호를 원자적으로 가져가며 처리. 호출 스레드도 같은 방식으로 참여한다
	struct FTask
	{
		const FParallel::FBody* Body = nullptr;
		uint32                  Count = 0;
		uint32                  Batch = 1;
		std::atomic<uint32>     NextBatch{ 0 };
		std::atomic<uint32>     DoneBatches{ 0 };
		uint32                  TotalBatches = 0;

		void Drain()
		{
			for (;;)
			{
				const uint32 BatchIndex = NextBatch.fetch_add(1, std::memory_order_relaxed);
				if (BatchIndex >= TotalBatches)
				{
					return;
				}
				const uint32 Begin = BatchIndex * Batch;
				const uint32 End   = std::min(Begin + Batch, Count);
				(*Body)(Begin, End);
				DoneBatches.fetch_add(1, std::memory_order_acq_rel);
			}
		}
	};

	class FParallelPool
	{
	public:
		static FParallelPool& Get()
		{
			static FParallelPool Pool;
			return Pool;
		}

		~FParallelPool() { Stop(); }

		uint32 GetWorkerCount()
		{
			EnsureStarted();
			return static_cast<uint32>(Workers.size());
		}

		void Run(FTask& Task)
		{
			EnsureStarted();
			{
				std::lock_guard Lock(Mutex);
				Current = &Task;
				++Generation;
			}
			WakeWorkers.notify_all();
			bInsideParallelFor = true;
			Task.Drain();
			bInsideParallelFor = false;
			// 다른 스레드가 가져간 묶음이 끝날 때까지 (짧으므로 양보하며 기다림)
			while (Task.DoneBatches.load(std::memory_order_acquire) < Task.TotalBatches)
			{
				std::this_thread::yield();
			}
			// 작업자가 Current를 더 이상 보지 않게 한 뒤 반환 (Task는 호출자 스택에 있다)
			std::unique_lock Lock(Mutex);
			Current = nullptr;
			Idle.wait(Lock, [this] { return Active == 0; });
		}

		void Stop()
		{
			{
				std::lock_guard Lock(Mutex);
				if (Workers.empty())
				{
					return;
				}
				bStopping = true;
			}
			WakeWorkers.notify_all();
			for (std::thread& Worker : Workers)
			{
				Worker.join();
			}
			Workers.clear();
			bStarted = false;
		}

	private:
		void EnsureStarted()
		{
			std::lock_guard Lock(Mutex);
			if (bStarted)
			{
				return;
			}
			bStarted   = true;
			bStopping  = false;
			const uint32 Hardware = std::max(1u, std::thread::hardware_concurrency());
			const uint32 Count    = std::min(Hardware > 1 ? Hardware - 1 : 0u, 15u);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				Workers.emplace_back([this] { WorkerMain(); });
			}
		}

		void WorkerMain()
		{
			E_PROFILE_THREAD_NAME("병렬 루프");
			uint64 SeenGeneration = 0;
			for (;;)
			{
				FTask* Task = nullptr;
				{
					std::unique_lock Lock(Mutex);
					WakeWorkers.wait(Lock, [&] { return bStopping || (Current != nullptr && Generation != SeenGeneration); });
					if (bStopping)
					{
						return;
					}
					SeenGeneration = Generation;
					Task           = Current;
					++Active;
				}
				bInsideParallelFor = true;
				Task->Drain();
				bInsideParallelFor = false;
				{
					std::lock_guard Lock(Mutex);
					--Active;
				}
				Idle.notify_all();
			}
		}

		std::mutex               Mutex;
		std::condition_variable  WakeWorkers;
		std::condition_variable  Idle;
		std::vector<std::thread> Workers;
		FTask*                   Current    = nullptr;
		uint64                   Generation = 0;
		uint32                   Active     = 0;
		bool                     bStarted   = false;
		bool                     bStopping  = false;
	};
} // namespace

namespace FParallel
{
	void ParallelFor(uint32 Count, uint32 MinBatch, const FBody& Body)
	{
		if (Count == 0)
		{
			return;
		}
		MinBatch = std::max(MinBatch, 1u);
		const bool bSequential = bInsideParallelFor || !CVarParallelFor.Get() || Count < 2 * MinBatch;
		FParallelPool& Pool    = FParallelPool::Get();
		const uint32   Workers = bSequential ? 0u : Pool.GetWorkerCount();
		if (Workers == 0)
		{
			Body(0, Count);
			return;
		}
		// 스레드당 묶음 4개 정도 (부하 불균형 흡수), 최소 MinBatch
		FTask Task;
		Task.Body         = &Body;
		Task.Count        = Count;
		Task.Batch        = std::max(MinBatch, (Count + (Workers + 1) * 4 - 1) / ((Workers + 1) * 4));
		Task.TotalBatches = (Count + Task.Batch - 1) / Task.Batch;
		Pool.Run(Task);
	}

	uint32 GetWorkerCount() { return FParallelPool::Get().GetWorkerCount(); }

	void Shutdown() { FParallelPool::Get().Stop(); }
} // namespace FParallel

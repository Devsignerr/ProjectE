#include "Core/Jobs/JobQueue.h"
#include "Core/Testing/TestFramework.h"

#include <atomic>
#include <chrono>

E_TEST(JobQueue_RunsWorkOnWorkersAndCompletionsOnPump)
{
	FJobQueue Jobs;
	Jobs.Init(3);
	const std::thread::id MainThread = std::this_thread::get_id();

	std::atomic<int32> WorkDone      = 0;
	std::atomic<int32> WorkOnMain    = 0;
	int32              Completions   = 0;
	int32              CompletionSum = 0;
	for (int32 Index = 0; Index < 64; ++Index)
	{
		auto Result = std::make_shared<int32>(0);
		Jobs.Submit(
			[&, Result, Index] {
				*Result = Index * 2;
				WorkOnMain += std::this_thread::get_id() == MainThread ? 1 : 0;
				++WorkDone;
			},
			[&, Result] {
				E_EXPECT_TRUE(std::this_thread::get_id() == MainThread);
				++Completions;
				CompletionSum += *Result;
			});
	}
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 64u);
	Jobs.WaitIdle();
	E_EXPECT_EQ(WorkDone.load(), 64);
	E_EXPECT_EQ(WorkOnMain.load(), 0);
	E_EXPECT_EQ(Completions, 0); // 완료 콜백은 Pump 전에는 실행되지 않는다
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 64u);

	E_EXPECT_EQ(Jobs.PumpCompletions(), 64u);
	E_EXPECT_EQ(Completions, 64);
	E_EXPECT_EQ(CompletionSum, 63 * 64); // 2 * (0 + ... + 63)
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 0u);
	Jobs.Shutdown();
}

E_TEST(JobQueue_ZeroWorkersRunsInline)
{
	FJobQueue Jobs;
	Jobs.Init(0);
	int32 Value       = 0;
	bool  bCompleted  = false;
	Jobs.Submit([&] { Value = 7; }, [&] { bCompleted = true; });
	E_EXPECT_EQ(Value, 7);
	E_EXPECT_FALSE(bCompleted);
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 1u);
	Jobs.PumpCompletions();
	E_EXPECT_TRUE(bCompleted);
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 0u);
}

E_TEST(JobQueue_CompletionCanSubmitMoreWork)
{
	FJobQueue Jobs;
	Jobs.Init(2);
	int32 Stage = 0;
	Jobs.Submit({}, [&] {
		Stage = 1;
		Jobs.Submit({}, [&] { Stage = 2; });
	});
	for (int32 Attempt = 0; Attempt < 100 && Stage < 2; ++Attempt)
	{
		Jobs.WaitIdle();
		Jobs.PumpCompletions();
	}
	E_EXPECT_EQ(Stage, 2);
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 0u);
}

E_TEST(JobQueue_ShutdownDropsQueuedWorkAndCompletions)
{
	FJobQueue Jobs;
	Jobs.Init(1);
	std::atomic<int32> Started   = 0;
	int32              Completed = 0;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Jobs.Submit(
			[&] {
				++Started;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			},
			[&] { ++Completed; });
	}
	Jobs.Shutdown(); // 실행 중 하나는 끝까지, 나머지는 버림
	E_EXPECT_TRUE(Started.load() < 20);
	Jobs.PumpCompletions();
	E_EXPECT_EQ(Completed, 0);
	E_EXPECT_EQ(Jobs.GetOutstandingCount(), 0u);
}

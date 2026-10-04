#include "Core/Jobs/ParallelFor.h"
#include "Core/Testing/TestFramework.h"

#include <atomic>
#include <thread>
#include <vector>

E_TEST(ParallelFor_CoversEveryIndexOnceAndIsDeterministic)
{
	for (const uint32 Count : { 0u, 1u, 7u, 100u, 10000u })
	{
		std::vector<uint32> Hits(Count, 0);
		std::vector<uint64> Values(Count, 0);
		std::atomic<bool>   bBadRange = false; // 작업자 스레드에서는 기대 매크로를 쓰지 않는다
		FParallel::ParallelFor(Count, 16, [&](uint32 Begin, uint32 End) {
			if (!(Begin < End && End <= Count))
			{
				bBadRange = true;
			}
			for (uint32 Index = Begin; Index < End; ++Index)
			{
				++Hits[Index];
				Values[Index] = static_cast<uint64>(Index) * Index;
			}
		});
		E_EXPECT_FALSE(bBadRange.load());
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			E_EXPECT_EQ(Hits[Index], 1u);
			E_EXPECT_EQ(Values[Index], static_cast<uint64>(Index) * Index);
		}
	}
}

E_TEST(ParallelFor_NestedRunsSequentiallyAndRepeatedCallsWork)
{
	std::atomic<uint32> Total = 0;
	for (int32 Round = 0; Round < 50; ++Round)
	{
		FParallel::ParallelFor(64, 1, [&](uint32 Begin, uint32 End) {
			for (uint32 Index = Begin; Index < End; ++Index)
			{
				// 중첩 호출은 이 스레드에서 순차로
				uint32 Inner = 0;
				FParallel::ParallelFor(10, 1, [&](uint32 InnerBegin, uint32 InnerEnd) { Inner += InnerEnd - InnerBegin; });
				Total.fetch_add(Inner, std::memory_order_relaxed);
			}
		});
	}
	E_EXPECT_EQ(Total.load(), 50u * 64u * 10u);
}

E_TEST(ParallelFor_BackToBackCallsNeverRunStaleBody)
{
	// 연속 호출: 늦게 깨어난 작업자가 이전 호출의 본문·범위를 다음 호출과 섞어 부르면 안 된다 (작업자는 호출 사이에 돌며 기다린다)
	std::atomic<bool> bWrongCall = false;
	for (uint32 Round = 0; Round < 3000; ++Round)
	{
		const uint32        Count = 50 + (Round * 37) % 900;
		std::vector<uint32> Hits(Count, 0);
		const uint32        Tag = Round;
		FParallel::ParallelFor(Count, 4, [&Hits, &bWrongCall, Count, Tag, Round](uint32 Begin, uint32 End) {
			if (Tag != Round || End > Count || Begin >= End)
			{
				bWrongCall = true;
				return;
			}
			for (uint32 Index = Begin; Index < End; ++Index)
			{
				++Hits[Index];
			}
		});
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			if (Hits[Index] != 1)
			{
				bWrongCall = true;
			}
		}
	}
	E_EXPECT_FALSE(bWrongCall.load());
}

// 두 스레드(게임 ↔ 렌더 스레드)가 동시에 불러도 모든 칸을 정확히 한 번씩 (늦게 온 쪽은 순차로)
E_TEST(ParallelFor_ConcurrentCallersFromTwoThreads)
{
	constexpr uint32       Count = 20000;
	std::vector<uint32>    HitsA(Count, 0);
	std::vector<uint32>    HitsB(Count, 0);
	const auto RunRounds = [](std::vector<uint32>& Hits) {
		for (int32 Round = 0; Round < 20; ++Round)
		{
			FParallel::ParallelFor(Count, 64, [&](uint32 Begin, uint32 End) {
				for (uint32 Index = Begin; Index < End; ++Index)
				{
					++Hits[Index];
				}
			});
		}
	};
	std::thread Other([&] { RunRounds(HitsB); });
	RunRounds(HitsA);
	Other.join();
	bool bAllExact = true;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		bAllExact = bAllExact && HitsA[Index] == 20u && HitsB[Index] == 20u;
	}
	E_EXPECT_TRUE(bAllExact);
}

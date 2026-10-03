#include "Core/Jobs/ParallelFor.h"
#include "Core/Testing/TestFramework.h"

#include <atomic>
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

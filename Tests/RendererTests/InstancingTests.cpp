#include "Core/Testing/TestFramework.h"
#include "Renderer/InstanceBatching.h"

#include <algorithm>

E_TEST(Instancing_SameKeyMergesIntoOneBatch)
{
	const uint64 KeyA = InstanceBatching::MakeKey(0, 1, 7, 0);
	const uint64 KeyB = InstanceBatching::MakeKey(0, 2, 7, 0);

	std::vector<FInstanceSortItem> Items = {
		{ KeyB, 5.0f, 0 }, { KeyA, 3.0f, 1 }, { KeyA, 1.0f, 2 }, { KeyB, 2.0f, 3 }, { KeyA, 2.0f, 4 },
	};
	std::vector<uint32>         Indices;
	std::vector<FInstanceBatch> Batches;
	InstanceBatching::Build(Items, Indices, Batches);

	// 키 순서(A < B), 묶음 안은 가까운 순
	E_EXPECT_EQ(Batches.size(), size_t(2));
	E_EXPECT_EQ(Batches[0].First, 0u);
	E_EXPECT_EQ(Batches[0].Count, 3u);
	E_EXPECT_EQ(Batches[1].First, 3u);
	E_EXPECT_EQ(Batches[1].Count, 2u);
	const std::vector<uint32> Expected = { 2, 4, 1, 3, 0 };
	E_EXPECT_TRUE(Indices == Expected);
	E_EXPECT_EQ(Batches[0].Instance, 2u); // 대표 = 묶음 첫 인스턴스
	E_EXPECT_EQ(Batches[1].Instance, 3u);
}

E_TEST(Instancing_UniqueKeysNeverMergeAndSortLast)
{
	const uint64 Static = InstanceBatching::MakeKey(7, 0xFFFFFF, 0xFFFFFF, 15); // 모든 필드 최댓값
	std::vector<FInstanceSortItem> Items = {
		{ InstanceBatching::MakeUniqueKey(9), 0.0f, 9 },
		{ Static, 10.0f, 1 },
		{ InstanceBatching::MakeUniqueKey(3), 0.0f, 3 },
		{ Static, 20.0f, 2 },
	};
	std::vector<uint32>         Indices;
	std::vector<FInstanceBatch> Batches;
	InstanceBatching::Build(Items, Indices, Batches);

	E_EXPECT_EQ(Batches.size(), size_t(3));
	E_EXPECT_EQ(Batches[0].Count, 2u); // 정적 묶음이 먼저
	E_EXPECT_EQ(Batches[1].Count, 1u);
	E_EXPECT_EQ(Batches[2].Count, 1u);
	E_EXPECT_EQ(Batches[1].Instance, 3u);
	E_EXPECT_EQ(Batches[2].Instance, 9u);
	E_EXPECT_TRUE(InstanceBatching::IsUniqueKey(InstanceBatching::MakeUniqueKey(0)));
	E_EXPECT_FALSE(InstanceBatching::IsUniqueKey(Static));
}

E_TEST(Instancing_KeyFieldsDoNotOverlap)
{
	// 필드마다 다른 키, LOD만 달라도 다른 묶음
	E_EXPECT_TRUE(InstanceBatching::MakeKey(0, 0, 0, 1) != InstanceBatching::MakeKey(0, 0, 0, 0));
	E_EXPECT_TRUE(InstanceBatching::MakeKey(0, 0, 1, 0) != InstanceBatching::MakeKey(0, 0, 0, 15));
	E_EXPECT_TRUE(InstanceBatching::MakeKey(0, 1, 0, 0) != InstanceBatching::MakeKey(0, 0, 0xFFFFFF, 15));
	E_EXPECT_TRUE(InstanceBatching::MakeKey(1, 0, 0, 0) != InstanceBatching::MakeKey(0, 0xFFFFFF, 0xFFFFFF, 15));
	// 정렬 우선순위: PSO > 머티리얼 > 메시 > LOD
	E_EXPECT_TRUE(InstanceBatching::MakeKey(0, 1, 0, 0) > InstanceBatching::MakeKey(0, 0, 0xFFFFFF, 15));
	E_EXPECT_TRUE(InstanceBatching::MakeKey(1, 0, 0, 0) > InstanceBatching::MakeKey(0, 0xFFFFFF, 0xFFFFFF, 15));
	E_EXPECT_FALSE(InstanceBatching::IsUniqueKey(InstanceBatching::MakeKey(7, 0xFFFFFF, 0xFFFFFF, 15)));

	std::vector<FInstanceSortItem> Empty;
	std::vector<uint32>            Indices = { 1 };
	std::vector<FInstanceBatch>    Batches = { {} };
	InstanceBatching::Build(Empty, Indices, Batches);
	E_EXPECT_TRUE(Indices.empty() && Batches.empty());
}

namespace
{
	// 키 묶음 정렬 대조용 기준 (예전 구현의 비교 정렬)
	std::vector<FInstanceSortItem> ReferenceSort(std::vector<FInstanceSortItem> Items)
	{
		std::sort(Items.begin(), Items.end(), [](const FInstanceSortItem& A, const FInstanceSortItem& B) {
			if (A.Key != B.Key)
			{
				return A.Key < B.Key;
			}
			if (A.Depth != B.Depth)
			{
				return A.Depth < B.Depth;
			}
			return A.Instance < B.Instance;
		});
		return Items;
	}

	bool SameOrder(const std::vector<FInstanceSortItem>& A, const std::vector<FInstanceSortItem>& B)
	{
		if (A.size() != B.size())
		{
			return false;
		}
		for (size_t Index = 0; Index < A.size(); ++Index)
		{
			if (A[Index].Key != B[Index].Key || A[Index].Instance != B[Index].Instance)
			{
				return false;
			}
		}
		return true;
	}

	uint32 NextRandom(uint32& State)
	{
		State = State * 1664525u + 1013904223u;
		return State >> 8;
	}
} // namespace

E_TEST(Instancing_GroupSortMatchesComparisonSort)
{
	// 번호 순 입력(패스들의 Add 순서) → 키 묶음 정렬 경로 (2048개 이상은 묶음 병렬). 키 몇 종류 + 같은 깊이·음수·±0·고유 키 섞음, 크기 여러 가지
	uint32 State = 12345u;
	for (uint32 Round = 0; Round < 40; ++Round)
	{
		const uint32 Count    = 1 + NextRandom(State) % 6000;
		const uint32 KeyKinds = 1 + NextRandom(State) % (Round % 4 == 0 ? 2000u : 40u);
		const bool   bZeroDepth = Round % 3 == 0; // 그림자처럼 깊이가 모두 0
		std::vector<FInstanceSortItem> Items;
		uint32 Instance = NextRandom(State) % 5;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			Instance += 1 + NextRandom(State) % 3;
			const uint32 Kind = NextRandom(State) % KeyKinds;
			uint64       Key  = InstanceBatching::MakeKey(Kind % 8, Kind * 7919u, Kind * 31u, Kind % 16);
			if (Kind % 97 == 5)
			{
				Key = InstanceBatching::MakeUniqueKey(Instance);
			}
			float Depth = 0.0f;
			if (!bZeroDepth)
			{
				const uint32 Pick = NextRandom(State) % 10;
				Depth = Pick == 0 ? -0.0f : Pick == 1 ? 0.0f : Pick == 2 ? 100.0f : static_cast<float>(NextRandom(State) % 200000) * 0.37f - 5000.0f;
			}
			Items.push_back({ Key, Depth, Instance });
		}
		const std::vector<FInstanceSortItem> Expected = ReferenceSort(Items);
		InstanceBatching::SortFrontToBack(Items);
		E_EXPECT_TRUE(SameOrder(Items, Expected));
	}
}

E_TEST(Instancing_UnorderedInputFallsBackToSameOrder)
{
	// 번호 순이 아닌 입력은 비교 정렬로 — 결과는 기준과 같다
	uint32 State = 777u;
	std::vector<FInstanceSortItem> Items;
	for (uint32 Index = 0; Index < 500; ++Index)
	{
		Items.push_back({ InstanceBatching::MakeKey(0, NextRandom(State) % 5, 1, 0), static_cast<float>(NextRandom(State) % 50), NextRandom(State) % 100000 });
	}
	const std::vector<FInstanceSortItem> Expected = ReferenceSort(Items);
	InstanceBatching::SortFrontToBack(Items);
	E_EXPECT_TRUE(SameOrder(Items, Expected));
}

E_TEST(Instancing_MergeSortedListsMatchesWholeSort)
{
	// 그림자 캐스터 조각처럼: 번호 구간을 나눠 조각마다 정렬 → 합침 = 전체를 한 번에 정렬 (빈 조각, 조각 하나, 깊이 0·임의 깊이)
	uint32 State = 4242u;
	for (uint32 Round = 0; Round < 30; ++Round)
	{
		const uint32 ListCount = Round % 7 == 0 ? 1u : 1u + NextRandom(State) % 12;
		const uint32 KeyKinds  = 1 + NextRandom(State) % 30;
		std::vector<std::vector<FInstanceSortItem>> Lists(ListCount);
		std::vector<FInstanceSortItem>              All;
		uint32                                      Instance = 0;
		for (uint32 ListIndex = 0; ListIndex < ListCount; ++ListIndex)
		{
			const uint32 Count = ListIndex % 5 == 3 ? 0u : NextRandom(State) % 900;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				Instance += 1 + NextRandom(State) % 2;
				const uint32 Kind  = NextRandom(State) % KeyKinds;
				const float  Depth = Round % 2 == 0 ? 0.0f : static_cast<float>(NextRandom(State) % 64);
				const FInstanceSortItem Item{ InstanceBatching::MakeKey(Kind % 4, 0, Kind, Kind % 3), Depth, Instance };
				Lists[ListIndex].push_back(Item);
				All.push_back(Item);
			}
			InstanceBatching::SortFrontToBack(Lists[ListIndex]);
		}
		std::vector<const std::vector<FInstanceSortItem>*> Pointers;
		for (const std::vector<FInstanceSortItem>& List : Lists)
		{
			Pointers.push_back(&List);
		}
		std::vector<FInstanceSortItem> Merged = { { 1, 1.0f, 1 } }; // 지워져야 한다
		InstanceBatching::MergeSortedLists(Pointers.data(), ListCount, Merged);
		E_EXPECT_TRUE(SameOrder(Merged, ReferenceSort(All)));
	}
}

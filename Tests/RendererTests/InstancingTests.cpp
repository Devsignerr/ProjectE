#include "Core/Testing/TestFramework.h"
#include "Renderer/InstanceBatching.h"

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

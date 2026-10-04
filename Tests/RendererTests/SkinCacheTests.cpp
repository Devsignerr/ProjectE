#include "Core/Testing/TestFramework.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/SkinCache.h"

#include <vector>

namespace
{
	// 메시 키는 주소만 비교한다 (배열 원소 = 서로 다른 주소)
	int        MeshKeys[3] = { 1, 2, 3 };
	const int& MeshA       = MeshKeys[0];
	const int& MeshB       = MeshKeys[1];
	const int& MeshC       = MeshKeys[2];
} // namespace

E_TEST(SkinCache_LayoutGroupsByMeshInFirstAppearanceOrder)
{
	// 입력: A(100) B(50) A(100) C(0 — 디스패치 없음) B(50) A(100)
	const std::vector<SkinCacheMath::FItemInput> Inputs = {
		{ &MeshA, 100, 100 }, { &MeshB, 50, 20 }, { &MeshA, 100, 100 }, { &MeshC, 0, 0 }, { &MeshB, 50, 20 }, { &MeshA, 100, 100 },
	};
	SkinCacheMath::FLayout Layout;
	SkinCacheMath::BuildLayout(Inputs, Layout);

	// 메시 첫 등장 순서 (A, B), 메시 안은 입력 순서
	const std::vector<uint32> ExpectedOrder = { 0, 2, 5, 1, 4 };
	E_EXPECT_TRUE(Layout.ItemOrder == ExpectedOrder);
	E_EXPECT_EQ(Layout.TotalVertices, 400ull);
	// 정점 자리 = 배치 순서 누적
	E_EXPECT_EQ(Layout.FirstVertex[0], 0u);
	E_EXPECT_EQ(Layout.FirstVertex[2], 100u);
	E_EXPECT_EQ(Layout.FirstVertex[5], 200u);
	E_EXPECT_EQ(Layout.FirstVertex[1], 300u);
	E_EXPECT_EQ(Layout.FirstVertex[4], 350u);
	E_EXPECT_EQ(Layout.FirstVertex[3], 0u); // 정점 0개 — 배치 안 함
	// 디스패치 = 메시 하나에 하나 (항목은 ItemOrder 연속 구간)
	E_EXPECT_EQ(Layout.Dispatches.size(), size_t{ 2 });
	E_EXPECT_EQ(Layout.Dispatches[0].FirstItem, 0u);
	E_EXPECT_EQ(Layout.Dispatches[0].ItemCount, 3u);
	E_EXPECT_EQ(Layout.Dispatches[0].VertexCount, 100u);
	E_EXPECT_EQ(Layout.Dispatches[1].FirstItem, 3u);
	E_EXPECT_EQ(Layout.Dispatches[1].ItemCount, 2u);
	E_EXPECT_EQ(Layout.Dispatches[1].VertexCount, 50u);
	E_EXPECT_EQ(Layout.Dispatches[1].ThreadCount, 20u); // LOD 목록 크기 (자리는 정점 수 전체)
}

E_TEST(SkinCache_LayoutRangesDoNotOverlap)
{
	// 여러 메시가 섞여도 [FirstVertex, + VertexCount) 범위가 겹치지 않고 빈틈 없이 TotalVertices를 채운다
	const int Meshes[3] = {};
	std::vector<SkinCacheMath::FItemInput> Inputs;
	for (uint32 Index = 0; Index < 37; ++Index)
	{
		const uint32 Mesh = (Index * 7) % 3;
		Inputs.push_back({ &Meshes[Mesh], 10u + Mesh * 13u, 10u + Mesh * 13u });
	}
	SkinCacheMath::FLayout Layout;
	SkinCacheMath::BuildLayout(Inputs, Layout);
	std::vector<int> Used(static_cast<size_t>(Layout.TotalVertices), 0);
	for (uint32 Index = 0; Index < Inputs.size(); ++Index)
	{
		for (uint32 Vertex = 0; Vertex < Inputs[Index].VertexCount; ++Vertex)
		{
			++Used[Layout.FirstVertex[Index] + Vertex];
		}
	}
	bool bExactlyOnce = true;
	for (const int Count : Used)
	{
		bExactlyOnce &= Count == 1;
	}
	E_EXPECT_TRUE(bExactlyOnce);
	// 디스패치 항목 합 = 입력 수, 같은 디스패치 항목은 같은 메시
	uint32 ItemSum = 0;
	bool   bSameMesh = true;
	for (const SkinCacheMath::FDispatch& Dispatch : Layout.Dispatches)
	{
		ItemSum += Dispatch.ItemCount;
		for (uint32 Item = 0; Item < Dispatch.ItemCount; ++Item)
		{
			bSameMesh &= Inputs[Layout.ItemOrder[Dispatch.FirstItem + Item]].MeshKey == Inputs[Layout.ItemOrder[Dispatch.FirstItem]].MeshKey;
		}
	}
	E_EXPECT_EQ(ItemSum, static_cast<uint32>(Inputs.size()));
	E_EXPECT_TRUE(bSameMesh);
}

E_TEST(SkinCache_LayoutSplitsDispatchAtGroupLimit)
{
	// 같은 메시 항목이 디스패치 Y 상한을 넘으면 여러 디스패치로 (자리 배치는 그대로 연속)
	std::vector<SkinCacheMath::FItemInput> Inputs(SkinCacheMath::MaxDispatchItems + 10, SkinCacheMath::FItemInput{ &MeshA, 3, 3 });
	SkinCacheMath::FLayout Layout;
	SkinCacheMath::BuildLayout(Inputs, Layout);
	E_EXPECT_EQ(Layout.Dispatches.size(), size_t{ 2 });
	E_EXPECT_EQ(Layout.Dispatches[0].ItemCount, SkinCacheMath::MaxDispatchItems);
	E_EXPECT_EQ(Layout.Dispatches[1].FirstItem, SkinCacheMath::MaxDispatchItems);
	E_EXPECT_EQ(Layout.Dispatches[1].ItemCount, 10u);
	E_EXPECT_EQ(Layout.FirstVertex.back(), static_cast<uint32>((Inputs.size() - 1) * 3));
}

E_TEST(SkinCache_CapacityGrowsAndNeverShrinks)
{
	using namespace SkinCacheMath;
	E_EXPECT_EQ(ComputeCapacity(0, 1), CapacityGranularity);                       // 단위로 올림
	E_EXPECT_EQ(ComputeCapacity(CapacityGranularity * 4, 100), CapacityGranularity * 4); // 줄이지 않음
	E_EXPECT_EQ(ComputeCapacity(CapacityGranularity * 4, CapacityGranularity * 4), CapacityGranularity * 4);
	// 넘으면 1.5배 이상 (작은 증가로 매 프레임 다시 만들지 않게)
	E_EXPECT_EQ(ComputeCapacity(CapacityGranularity * 4, CapacityGranularity * 4 + 1), CapacityGranularity * 6);
	// 1.5배보다 큰 요구는 요구량을 단위로 올림
	E_EXPECT_EQ(ComputeCapacity(CapacityGranularity, CapacityGranularity * 10 + 5), CapacityGranularity * 11);
	E_EXPECT_EQ(ComputeCapacity(MaxCapacity - CapacityGranularity, MaxCapacity), MaxCapacity); // 상한에서 자름
	E_EXPECT_EQ(ComputeCapacity(0, MaxCapacity + 1), 0ull);                            // 불가
	static_assert(MaxCapacity * MaxBytesPerVertex <= 0xFFFFFFFFull, "바이트 주소가 uint32 안");
}

E_TEST(SkinCache_RegionsAreContiguousAndFitBuffer)
{
	// 영역 순서: 위치(16B) → 법선·탄젠트(32B) → 이전 위치(16B) → RT 정점(FVertex 64B, 있을 때만)
	using namespace SkinCacheMath;
	const uint64 Capacity = 1000;
	E_EXPECT_EQ(GetTangentOffset(Capacity), 16000ull);
	E_EXPECT_EQ(GetPrevOffset(Capacity), 48000ull);
	E_EXPECT_EQ(GetRtOffset(Capacity), 64000ull);
	E_EXPECT_EQ(GetPrevOffset(Capacity) + Capacity * PrevBytes, GetBufferBytes(Capacity, 0)); // 래스터 영역 끝 = 버퍼 끝
	E_EXPECT_EQ(GetRtOffset(Capacity) + 300 * RtVertexBytes, GetBufferBytes(Capacity, 300));  // RT 정점 영역은 따로 센 용량
	// 인스턴스 GPU 데이터 크기는 그대로 (캐시 칸 2개가 예전 여백 자리)
	static_assert(sizeof(FInstanceGpuData) == 192);
}

E_TEST(SkinCache_LodVertexListsAreUnionOfCoarserLods)
{
	// 정점 6개: LOD0 = 0~5 전부 (항등), LOD1 = {0,2,4}, LOD2 = {4,5,0}
	const std::vector<uint32> Indices = { 0, 1, 2, 3, 4, 5, /* LOD1 */ 0, 2, 4, /* LOD2 */ 4, 5, 0 };
	const std::vector<std::pair<uint32, uint32>> Ranges = { { 0, 6 }, { 6, 3 }, { 9, 3 } };
	std::vector<std::vector<uint32>> Lists;
	SkinCacheMath::BuildLodVertexLists(6, Indices, Ranges, Lists);
	E_EXPECT_EQ(Lists.size(), size_t{ 3 });
	E_EXPECT_TRUE(Lists[0].empty()); // 모든 정점 = 항등
	// LOD1 이상 = LOD1 ∪ LOD2 (그림자 LOD 바이어스가 더 거친 LOD를 그려도 덮임), 오름차순
	const std::vector<uint32> Expected1 = { 0, 2, 4, 5 };
	const std::vector<uint32> Expected2 = { 0, 4, 5 };
	E_EXPECT_TRUE(Lists[1] == Expected1);
	E_EXPECT_TRUE(Lists[2] == Expected2);
}

E_TEST(SkinCache_LodVertexListKeepsUnusedVertexOutOfLod0)
{
	// LOD0이 쓰지 않는 정점(3)이 있으면 LOD0 목록도 항등이 아니다
	const std::vector<uint32> Indices = { 0, 1, 2 };
	std::vector<std::vector<uint32>> Lists;
	SkinCacheMath::BuildLodVertexLists(4, Indices, { { 0, 3 } }, Lists);
	const std::vector<uint32> Expected = { 0, 1, 2 };
	E_EXPECT_TRUE(Lists[0] == Expected);
}

#include "Core/Testing/TestFramework.h"
#include "Editor/TerrainEditHistory.h"

// 지형 편집 버전 사슬 (Editor/TerrainEditHistory.h): 되감기/다시 따라가기, Undo 뒤 새 편집(갈래), 모르는 버전
namespace
{
	// 정점 (X, Y) 높이를 Value로 바꾸는 "스트로크" 하나를 기록하고 새 버전을 돌려준다
	uint32 Stroke(FTerrainEditHistory& History, FTerrainData& Data, int32 X, int32 Y, uint16 Value)
	{
		const FTerrainRect   Rect{ X, Y, X, Y };
		FTerrainRegion       Before = FTerrainRegion::Capture(Data, Rect);
		Data.Heights[static_cast<size_t>(Y) * Data.Resolution + X] = Value;
		FTerrainRegion After    = FTerrainRegion::Capture(Data, Rect);
		const uint32   Revision = History.MakeRevision();
		History.Record("A.eterrain", Data, Revision, std::move(Before), std::move(After));
		return Revision;
	}
} // namespace

E_TEST(TerrainEditHistory_UndoRedoChain)
{
	FTerrainData Data;
	Data.Initialize(9);
	FTerrainEditHistory History;
	const uint32        R0 = Data.Revision;
	const uint32        R1 = Stroke(History, Data, 1, 1, 100);
	const uint32        R2 = Stroke(History, Data, 2, 2, 200);
	E_EXPECT_TRUE(R1 != R0 && R2 != R1);
	E_EXPECT_EQ(Data.Revision, R2);

	E_EXPECT_TRUE(History.Reconcile("A.eterrain", Data, R0)); // 두 단계 되감기
	E_EXPECT_EQ(Data.GetHeight(1, 1), FTerrainData::DefaultHeight);
	E_EXPECT_EQ(Data.GetHeight(2, 2), FTerrainData::DefaultHeight);
	E_EXPECT_TRUE(History.Reconcile("A.eterrain", Data, R2)); // 두 단계 다시
	E_EXPECT_EQ(Data.GetHeight(1, 1), uint16(100));
	E_EXPECT_EQ(Data.GetHeight(2, 2), uint16(200));
	E_EXPECT_TRUE(History.Reconcile("A.eterrain", Data, R1));
	E_EXPECT_EQ(Data.GetHeight(2, 2), FTerrainData::DefaultHeight);
	E_EXPECT_EQ(Data.Revision, R1);
}

E_TEST(TerrainEditHistory_BranchAfterUndo)
{
	FTerrainData Data;
	Data.Initialize(9);
	FTerrainEditHistory History;
	const uint32        R1 = Stroke(History, Data, 1, 1, 100);
	const uint32        R2 = Stroke(History, Data, 2, 2, 200);
	E_EXPECT_TRUE(History.Reconcile("A.eterrain", Data, R1));
	const uint32 R3 = Stroke(History, Data, 3, 3, 300); // R1에서 갈라진 새 편집
	E_EXPECT_EQ(Data.GetHeight(2, 2), FTerrainData::DefaultHeight);

	// 다른 갈래(R2)로 이동: 공통 조상 R1까지 되감고 R2를 따라간다
	E_EXPECT_TRUE(History.Reconcile("A.eterrain", Data, R2));
	E_EXPECT_EQ(Data.GetHeight(3, 3), FTerrainData::DefaultHeight);
	E_EXPECT_EQ(Data.GetHeight(2, 2), uint16(200));
	E_EXPECT_TRUE(History.Reconcile("A.eterrain", Data, R3));
	E_EXPECT_EQ(Data.GetHeight(3, 3), uint16(300));
	E_EXPECT_EQ(Data.GetHeight(2, 2), FTerrainData::DefaultHeight);
	E_EXPECT_EQ(Data.GetHeight(1, 1), uint16(100));

	// 기록에 없는 버전 / 다른 에셋은 데이터를 건드리지 않는다
	E_EXPECT_FALSE(History.Reconcile("A.eterrain", Data, 12345u));
	E_EXPECT_EQ(Data.Revision, R3);
	E_EXPECT_FALSE(History.Reconcile("B.eterrain", Data, R1));
	E_EXPECT_EQ(Data.GetHeight(3, 3), uint16(300));
}

E_TEST(TerrainEditHistory_TrimsOldestRecords)
{
	FTerrainData Data;
	Data.Initialize(9);
	FTerrainEditHistory History;
	History.SetMaxBytes(64); // 레코드 하나 = 전/후 각 (2 + 4) 바이트 = 12바이트
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Stroke(History, Data, Index % 9, 0, static_cast<uint16>(Index));
	}
	E_EXPECT_TRUE(History.GetMemorySize() <= 64);
	E_EXPECT_TRUE(History.GetRecordCount() < 10);
}

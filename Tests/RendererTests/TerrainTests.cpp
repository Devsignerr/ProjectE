#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Renderer/TerrainRenderer.h"
#include "Scene/Terrain.h"

#include <cmath>

// 지형 순수 로직 (Scene/Terrain.h): 좌표 변환, 높이 보간, 법선, 레이캐스트, 브러시, 가중치, 영역 기록, 직렬화
namespace
{
	FTerrainComponent MakeComponent(float Size = 800.0f, float Range = 6553.5f)
	{
		FTerrainComponent Terrain;
		Terrain.Size        = FVector2(Size, Size);
		Terrain.HeightRange = Range; // 1 단위 = 0.1cm
		return Terrain;
	}
}

E_TEST(Terrain_FrameMapsCenterAndHeight)
{
	FTerrainData Data;
	Data.Initialize(9); // 8 x 8 셀
	const FTerrainComponent Terrain = MakeComponent();
	const FTerrainFrame     Frame   = FTerrainFrame::Make(FVector3(1000.0f, -200.0f, 50.0f), Terrain, Data.Resolution);
	E_EXPECT_NEAR(Frame.CellSize.X, 100.0f, 1.0e-4f);
	// 가운데 정점 (4, 4) = 엔티티 위치, 기본 높이 32768 ≈ 위치 Z
	const FVector3 Center = Frame.GridToWorld(4.0f, 4.0f, FTerrainData::DefaultHeight);
	E_EXPECT_NEAR(Center.X, 1000.0f, 1.0e-3f);
	E_EXPECT_NEAR(Center.Y, -200.0f, 1.0e-3f);
	E_EXPECT_NEAR(Center.Z, 50.0f, 0.1f);
	const FVector2 Grid = Frame.WorldToGrid(Center.X + 150.0f, Center.Y - 50.0f);
	E_EXPECT_NEAR(Grid.X, 5.5f, 1.0e-4f);
	E_EXPECT_NEAR(Grid.Y, 3.5f, 1.0e-4f);
	E_EXPECT_NEAR(TerrainMath::SampleWorldHeight(Data, Frame, Center.X, Center.Y), 50.0f, 0.1f);
}

E_TEST(Terrain_SampleHeightUsesCellTriangles)
{
	FTerrainData Data;
	Data.Initialize(3);
	Data.Heights = { 0, 100, 0, 100, 200, 0, 0, 0, 0 }; // 행 우선 (Y * 3 + X)
	E_EXPECT_NEAR(TerrainMath::SampleHeight(Data, 1.0f, 0.0f), 100.0f, 1.0e-3f);
	E_EXPECT_NEAR(TerrainMath::SampleHeight(Data, 1.0f, 1.0f), 200.0f, 1.0e-3f);
	// 셀 (0,0): 대각선 (0,0)-(1,1) 위 = 두 끝 평균
	E_EXPECT_NEAR(TerrainMath::SampleHeight(Data, 0.5f, 0.5f), 100.0f, 1.0e-3f);
	// 아래 삼각형 (FX >= FY): (0,0),(1,0),(1,1)
	E_EXPECT_NEAR(TerrainMath::SampleHeight(Data, 0.75f, 0.25f), 0.75f * 100.0f + 0.25f * 100.0f, 1.0e-3f);
	// 격자 밖은 가장자리로
	E_EXPECT_NEAR(TerrainMath::SampleHeight(Data, -5.0f, 1.0f), 100.0f, 1.0e-3f);
}

E_TEST(Terrain_NormalFollowsSlope)
{
	FTerrainData Data;
	Data.Initialize(5);
	const FTerrainComponent Terrain = MakeComponent(400.0f); // 셀 100cm, 1 단위 = 0.1cm
	const FTerrainFrame     Frame   = FTerrainFrame::Make(FVector3(), Terrain, Data.Resolution);
	E_EXPECT_TRUE(TerrainMath::ComputeNormal(Data, Frame, 2.0f, 2.0f).Equals(FVector3::UpVector, 1.0e-5f));

	// +X로 셀마다 100cm 오름 (45도) → 법선 (-1, 0, 1)/√2
	for (int32 Y = 0; Y < 5; ++Y)
	{
		for (int32 X = 0; X < 5; ++X)
		{
			Data.Heights[Y * 5 + X] = static_cast<uint16>(10000 + X * 1000);
		}
	}
	const FVector3 Normal = TerrainMath::ComputeNormal(Data, Frame, 2.0f, 2.0f);
	E_EXPECT_NEAR(Normal.X, -0.70710678f, 1.0e-4f);
	E_EXPECT_NEAR(Normal.Y, 0.0f, 1.0e-4f);
	E_EXPECT_NEAR(Normal.Z, 0.70710678f, 1.0e-4f);
}

E_TEST(Terrain_RaycastHitsSurface)
{
	FTerrainData Data;
	Data.Initialize(17);
	Data.Heights[8 * 17 + 8] = 40000; // 가운데 봉우리
	const FTerrainComponent Terrain = MakeComponent(1600.0f);
	const FTerrainFrame     Frame   = FTerrainFrame::Make(FVector3(), Terrain, Data.Resolution);

	float Distance = 0.0f;
	E_EXPECT_TRUE(TerrainMath::Raycast(Data, Frame, FVector3(0.0f, 0.0f, 5000.0f), FVector3(0.0f, 0.0f, -1.0f), 10000.0f, Distance));
	E_EXPECT_NEAR(5000.0f - Distance, Frame.HeightToWorldZ(40000.0f), 0.05f);

	// 비스듬한 광선: 맞은 점이 표면 위에 있어야 한다
	const FVector3 Origin(-900.0f, 300.0f, 2000.0f);
	const FVector3 Direction = (FVector3(250.0f, 0.0f, 0.0f) - Origin).GetNormalized();
	E_EXPECT_TRUE(TerrainMath::Raycast(Data, Frame, Origin, Direction, 10000.0f, Distance));
	const FVector3 Hit = Origin + Direction * Distance;
	E_EXPECT_NEAR(Hit.Z, TerrainMath::SampleWorldHeight(Data, Frame, Hit.X, Hit.Y), 0.5f);

	// 하늘로 쏘면 못 맞힘
	E_EXPECT_FALSE(TerrainMath::Raycast(Data, Frame, FVector3(0.0f, 0.0f, 5000.0f), FVector3(0.0f, 0.0f, 1.0f), 10000.0f, Distance));
}

E_TEST(Terrain_BrushFalloff)
{
	E_EXPECT_NEAR(TerrainMath::BrushFalloff(0.0f, 0.5f), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(TerrainMath::BrushFalloff(0.4f, 0.5f), 1.0f, 1.0e-6f); // 단단한 안쪽
	E_EXPECT_NEAR(TerrainMath::BrushFalloff(0.75f, 0.5f), 0.5f, 1.0e-6f); // 감쇠 구간 중간
	E_EXPECT_NEAR(TerrainMath::BrushFalloff(1.0f, 0.5f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(TerrainMath::BrushFalloff(0.99f, 0.0f), 1.0f, 1.0e-6f); // 감쇠 없음
	float Previous = 1.0f;
	for (int32 Step = 0; Step <= 20; ++Step)
	{
		const float Value = TerrainMath::BrushFalloff(static_cast<float>(Step) / 20.0f, 1.0f);
		E_EXPECT_TRUE(Value <= Previous + 1.0e-6f);
		Previous = Value;
	}
}

E_TEST(Terrain_PaintWeightKeepsSum)
{
	auto Sum = [](uint32 Packed) {
		int32 Total = 0;
		for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
		{
			Total += TerrainMath::GetLayerWeight(Packed, Layer);
		}
		return Total;
	};
	uint32 Weight = 255u; // 레이어 0만
	for (int32 Step = 0; Step < 10; ++Step)
	{
		Weight = TerrainMath::PaintWeight(Weight, 2, 0.3f);
		E_EXPECT_EQ(Sum(Weight), 255);
	}
	E_EXPECT_TRUE(TerrainMath::GetLayerWeight(Weight, 2) > 240);
	Weight = TerrainMath::PaintWeight(Weight, 1, 0.5f);
	E_EXPECT_EQ(Sum(Weight), 255);
	E_EXPECT_TRUE(TerrainMath::GetLayerWeight(Weight, 1) >= 127);
	E_EXPECT_EQ(TerrainMath::PaintWeight(Weight, 3, 1.0f), 0xFF000000u);
	E_EXPECT_EQ(Sum(TerrainMath::NormalizeWeight(0x00404040u)), 255);
	E_EXPECT_EQ(TerrainMath::NormalizeWeight(0u), 255u);
}

E_TEST(Terrain_BrushOpsChangeHeights)
{
	FTerrainData Data;
	Data.Initialize(33);
	const FTerrainComponent Terrain = MakeComponent(3200.0f, 5000.0f); // 셀 100cm
	const FTerrainFrame     Frame   = FTerrainFrame::Make(FVector3(), Terrain, Data.Resolution);
	const int32             Center  = 16 * 33 + 16;

	FTerrainBrush Brush;
	Brush.Op       = ETerrainBrushOp::Raise;
	Brush.Radius   = 400.0f;
	Brush.Strength = 1.0f;
	Brush.Falloff  = 0.5f;
	const FTerrainRect Rect = ApplyTerrainBrush(Data, Frame, Brush, FVector2(0.0f, 0.0f), 0.1f);
	E_EXPECT_FALSE(Rect.IsEmpty());
	E_EXPECT_TRUE(Rect.MinX >= 12 && Rect.MaxX <= 20);
	E_EXPECT_TRUE(Data.Heights[Center] > FTerrainData::DefaultHeight);
	// 세기 1, 0.1초 = 100cm
	E_EXPECT_NEAR(Frame.HeightToWorldZ(Data.Heights[Center]) - Frame.HeightToWorldZ(FTerrainData::DefaultHeight), 100.0f, 0.2f);
	E_EXPECT_EQ(Data.Heights[0], FTerrainData::DefaultHeight); // 반경 밖

	// 부드럽게: 봉우리 높이가 낮아진다
	const uint16 Peak = Data.Heights[Center];
	Brush.Op = ETerrainBrushOp::Smooth;
	Brush.Radius = 800.0f;
	for (int32 Step = 0; Step < 30; ++Step)
	{
		ApplyTerrainBrush(Data, Frame, Brush, FVector2(0.0f, 0.0f), 1.0f / 30.0f);
	}
	E_EXPECT_TRUE(Data.Heights[Center] < Peak);

	// 평탄화: 목표 높이로 수렴
	Brush.Op            = ETerrainBrushOp::Flatten;
	Brush.FlattenHeight = -300.0f;
	for (int32 Step = 0; Step < 120; ++Step)
	{
		ApplyTerrainBrush(Data, Frame, Brush, FVector2(0.0f, 0.0f), 1.0f / 30.0f);
	}
	E_EXPECT_NEAR(Frame.HeightToWorldZ(Data.Heights[Center]), -300.0f, 1.0f);

	// 칠하기
	Brush.Op    = ETerrainBrushOp::Paint;
	Brush.Layer = 1;
	ApplyTerrainBrush(Data, Frame, Brush, FVector2(0.0f, 0.0f), 0.5f);
	E_EXPECT_TRUE(TerrainMath::GetLayerWeight(Data.Weights[Center], 1) > 100);
	E_EXPECT_EQ(Data.Weights[0], 255u);
}

E_TEST(Terrain_RegionCaptureApplyRestores)
{
	FTerrainData Data;
	Data.Initialize(17);
	const std::vector<uint16> Before = Data.Heights;
	const FTerrainRect        Rect{ 3, 4, 9, 6 };
	const FTerrainRegion      Region = FTerrainRegion::Capture(Data, Rect);
	E_EXPECT_EQ(Region.Heights.size(), size_t(7 * 3));
	for (uint16& Height : Data.Heights)
	{
		Height = 1234;
	}
	Region.Apply(Data);
	for (int32 Y = 0; Y < 17; ++Y)
	{
		for (int32 X = 0; X < 17; ++X)
		{
			const bool bInside = X >= 3 && X <= 9 && Y >= 4 && Y <= 6;
			E_EXPECT_EQ(Data.GetHeight(X, Y), bInside ? Before[Y * 17 + X] : uint16(1234));
		}
	}
}

E_TEST(Terrain_ChangeTrackingAndRect)
{
	FTerrainData Data;
	Data.Initialize(65);
	const uint64 Seen = Data.ChangeCounter;
	FTerrainRect Changes;
	E_EXPECT_FALSE(Data.GetChangesSince(Seen, Changes));
	Data.MarkChanged({ 1, 2, 3, 4 });
	Data.MarkChanged({ 10, 0, 12, 1 });
	E_EXPECT_TRUE(Data.GetChangesSince(Seen, Changes));
	E_EXPECT_EQ(Changes.MinX, 1);
	E_EXPECT_EQ(Changes.MinY, 0);
	E_EXPECT_EQ(Changes.MaxX, 12);
	E_EXPECT_EQ(Changes.MaxY, 4);
	// 아주 오래된 카운터 → 전체
	E_EXPECT_TRUE(Data.GetChangesSince(0, Changes));
	E_EXPECT_EQ(Changes.MaxX, 64);
}

E_TEST(Terrain_JsonAndBase64RoundTrip)
{
	const uint8         Bytes[] = { 0, 1, 2, 250, 251, 252, 253 };
	std::vector<uint8>  Decoded;
	for (size_t Length = 0; Length <= sizeof(Bytes); ++Length)
	{
		E_EXPECT_TRUE(TerrainIO::DecodeBase64(TerrainIO::EncodeBase64(Bytes, Length), Decoded));
		E_EXPECT_EQ(Decoded.size(), Length);
		E_EXPECT_TRUE(std::equal(Decoded.begin(), Decoded.end(), Bytes));
	}

	FTerrainData Data;
	Data.Initialize(9);
	Data.Heights[5]  = 777;
	Data.Weights[7]  = 0x01020304u;
	Data.Revision    = 42;
	FTerrainData Loaded;
	std::string  Error;
	E_EXPECT_TRUE(TerrainIO::FromJsonString(TerrainIO::ToJsonString(Data), Loaded, &Error));
	E_EXPECT_EQ(Loaded.Resolution, 9u);
	E_EXPECT_EQ(Loaded.Revision, 42u);
	E_EXPECT_TRUE(Loaded.Heights == Data.Heights);
	E_EXPECT_TRUE(Loaded.Weights == Data.Weights);
	E_EXPECT_FALSE(TerrainIO::FromJsonString("{\"Resolution\":1}", Loaded, &Error));
}

E_TEST(Terrain_Png16AndResample)
{
	const std::vector<uint16> Pixels = { 0, 65535, 1000, 2000 };
	const std::vector<uint8>  Png    = TerrainIO::EncodePng16(Pixels, 2, 2);
	E_EXPECT_TRUE(Png.size() > 8 + 25 + 12);
	E_EXPECT_EQ(Png[0], uint8(0x89));
	E_EXPECT_EQ(Png[1], uint8('P'));
	E_EXPECT_EQ(Png[24], uint8(16)); // IHDR 비트 깊이

	const std::vector<uint16> Resampled = TerrainIO::ResampleHeights({ 0, 1000, 2000, 3000 }, 2, 2, 3);
	E_EXPECT_EQ(Resampled.size(), size_t(9));
	E_EXPECT_EQ(Resampled[0], uint16(0));
	E_EXPECT_EQ(Resampled[2], uint16(1000));
	E_EXPECT_EQ(Resampled[4], uint16(1500));
	E_EXPECT_EQ(Resampled[8], uint16(3000));
}

E_TEST(Terrain_ComponentSerializes)
{
	FScene  Scene;
	FEntity Entity                  = Scene.CreateEntity("Ground");
	FTerrainComponent& Terrain      = Scene.GetRegistry().Emplace<FTerrainComponent>(Entity);
	Terrain.Asset                   = "Terrain/Test.eterrain";
	Terrain.Layer2Material          = "Materials/Rock.emat";
	Terrain.EditRevision            = 99;
	const std::string Json          = FSceneSerializer::ToJsonString(Scene);
	FScene            Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, Json));
	bool bFound = false;
	Loaded.GetRegistry().View<FTerrainComponent>().Each([&](FEntity, FTerrainComponent& Component) {
		bFound = Component.Asset == "Terrain/Test.eterrain" && Component.Layer2Material == "Materials/Rock.emat" && Component.EditRevision == 99;
	});
	E_EXPECT_TRUE(bFound);
}

E_TEST(TerrainRenderer_ChunkSizeAndLod)
{
	E_EXPECT_EQ(FTerrainRenderer::ComputeChunkCells(512), 64u);
	E_EXPECT_EQ(FTerrainRenderer::ComputeChunkCells(96), 32u);
	E_EXPECT_EQ(FTerrainRenderer::ComputeChunkCells(8), 8u);
	E_EXPECT_EQ(FTerrainRenderer::ComputeChunkCells(7), 0u); // 홀수 셀 수는 그릴 수 없다
	// 청크 크기 1000cm: 1000 안 = LOD0, 1000~2000 = 1, 2000~4000 = 2, 상한
	E_EXPECT_EQ(FTerrainRenderer::SelectChunkLod(0.0f, 1000.0f, 1.0f, 5), 0u);
	E_EXPECT_EQ(FTerrainRenderer::SelectChunkLod(999.0f, 1000.0f, 1.0f, 5), 0u);
	E_EXPECT_EQ(FTerrainRenderer::SelectChunkLod(1500.0f, 1000.0f, 1.0f, 5), 1u);
	E_EXPECT_EQ(FTerrainRenderer::SelectChunkLod(3000.0f, 1000.0f, 1.0f, 5), 2u);
	E_EXPECT_EQ(FTerrainRenderer::SelectChunkLod(1.0e7f, 1000.0f, 1.0f, 5), 5u);
	// 배율 2 = 같은 LOD를 두 배 멀리까지
	E_EXPECT_EQ(FTerrainRenderer::SelectChunkLod(1500.0f, 1000.0f, 2.0f, 5), 0u);
}

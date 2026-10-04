#include "Core/Paths.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/Sprite/FlipbookSystem.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"
#include "Scene/Sprite/TilemapCollision.h"
#include "Scene/Sprite/TilemapData.h"
#include "Scene/Terrain.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	constexpr float Tolerance = 1.0e-5f;

	bool Near(const FVector2& A, const FVector2& B, float Tol = Tolerance)
	{
		return std::abs(A.X - B.X) <= Tol && std::abs(A.Y - B.Y) <= Tol;
	}

	// 기준 플립북 프레임 길이 0.1 / 0.2 / 0.3 (전체 0.6), 이벤트 A(0) B(1) C(2) + 범위 밖 X(7)
	const std::vector<float>          TestDurations = { 0.1f, 0.2f, 0.3f };
	const std::vector<FFlipbookEvent> TestEvents    = { { 0, "A" }, { 1, "B" }, { 2, "C" }, { 7, "X" } };

	std::vector<int32> Collect(float Prev, float New, EFlipbookLoopMode Mode, bool bIncludeStart = false)
	{
		std::vector<int32> Result;
		FlipbookMath::CollectEvents(Prev, New, TestDurations, Mode, TestEvents, bIncludeStart, Result);
		return Result;
	}

	fs::path SampleDirectory()
	{
		return FPaths::GetEngineDirectory() / L"Projects" / L"Sample" / L"Content" / L"Sprites" / L"Samples";
	}

	std::string SamplePath(const wchar_t* File)
	{
		return (SampleDirectory() / File).generic_string();
	}
} // namespace

// ---- 스프라이트 --------------------------------------------------------------------------------------------------------

E_TEST(Sprite_SliceGridMarginSpacing)
{
	// 샘플 아틀라스와 같은 배치: 34px 칸 4x2, 여백 1, 간격 2 → 144x72
	const std::vector<FSpriteSlice> Slices = SpriteMath::SliceGrid(144, 72, 34, 34, 1, 2, "Icon_");
	E_EXPECT_EQ(Slices.size(), static_cast<size_t>(8));
	E_EXPECT_EQ(Slices[0].X, 1);
	E_EXPECT_EQ(Slices[0].Y, 1);
	E_EXPECT_EQ(Slices[5].Name, std::string("Icon_5")); // 행 우선: 5 = 1행 1열
	E_EXPECT_EQ(Slices[5].X, 37);
	E_EXPECT_EQ(Slices[5].Y, 37);
	E_EXPECT_EQ(Slices[7].X + Slices[7].W, 143); // 오른쪽 여백 1
	E_EXPECT_TRUE(SpriteMath::SliceGrid(144, 72, 0, 34, 0, 0, "").empty());
	E_EXPECT_EQ(SpriteMath::SliceGrid(33, 33, 34, 34, 0, 0, "").size(), static_cast<size_t>(0)); // 칸보다 작은 텍스처
}

E_TEST(Sprite_UvRectExactTexelEdges)
{
	FSpriteSlice Slice;
	Slice.X = 37;
	Slice.Y = 37;
	Slice.W = 34;
	Slice.H = 34;
	const FSpriteUvRect Uv = SpriteMath::ComputeUvRect(Slice, 144, 72);
	E_EXPECT_NEAR(Uv.U0, 37.0f / 144.0f, Tolerance);
	E_EXPECT_NEAR(Uv.V0, 37.0f / 72.0f, Tolerance);
	E_EXPECT_NEAR(Uv.U1, 71.0f / 144.0f, Tolerance);
	E_EXPECT_NEAR(Uv.V1, 71.0f / 72.0f, Tolerance);
	E_EXPECT_TRUE(SpriteMath::ComputeUvRect(Slice, 0, 72) == FSpriteUvRect{});
}

E_TEST(Sprite_QuadPivotUnitsAndFlip)
{
	FSpriteSlice Slice;
	Slice.W     = 32;
	Slice.H     = 16;
	Slice.Pivot = FVector2(0.25f, 0.0f); // 왼쪽 1/4, 아래
	// UnitsPerPixel 2 → 64 x 32 cm. 텍스처 64x16 → U 0~0.5, V 0~1
	const FSpriteQuad Quad = SpriteMath::ComputeQuad(Slice, 64, 16, 2.0f, FVector2::ZeroVector, false, false);
	E_EXPECT_TRUE(Near(Quad.Positions[0], FVector2(-16.0f, 0.0f)));  // 왼쪽 아래
	E_EXPECT_TRUE(Near(Quad.Positions[2], FVector2(48.0f, 32.0f)));  // 오른쪽 위
	E_EXPECT_TRUE(Near(Quad.Uvs[0], FVector2(0.0f, 1.0f)));          // 사각형 아래 = 이미지 아래(V1)
	E_EXPECT_TRUE(Near(Quad.Uvs[2], FVector2(0.5f, 0.0f)));

	// 좌우 반전: 피벗 축 거울 → 왼쪽 -48 ~ 오른쪽 16, 왼쪽 아래 정점이 이미지 오른쪽(U1)을 갖는다. 정점 공간 순서는 그대로
	const FSpriteQuad FlipX = SpriteMath::ComputeQuad(Slice, 64, 16, 2.0f, FVector2::ZeroVector, true, false);
	E_EXPECT_TRUE(Near(FlipX.Positions[0], FVector2(-48.0f, 0.0f)));
	E_EXPECT_TRUE(Near(FlipX.Positions[1], FVector2(16.0f, 0.0f)));
	E_EXPECT_TRUE(Near(FlipX.Uvs[0], FVector2(0.5f, 1.0f)));
	E_EXPECT_TRUE(Near(FlipX.Uvs[1], FVector2(0.0f, 1.0f)));

	// 상하 반전: 아래 피벗 → 사각형이 원점 아래로 (Z -32 ~ 0), 아래 정점이 이미지 위(V0)
	const FSpriteQuad FlipY = SpriteMath::ComputeQuad(Slice, 64, 16, 2.0f, FVector2::ZeroVector, false, true);
	E_EXPECT_TRUE(Near(FlipY.Positions[0], FVector2(-16.0f, -32.0f)));
	E_EXPECT_TRUE(Near(FlipY.Positions[3], FVector2(-16.0f, 0.0f)));
	E_EXPECT_TRUE(Near(FlipY.Uvs[0], FVector2(0.0f, 0.0f)));

	// 크기: 한 축만 주면 비율 유지, 둘 다 주면 그대로
	E_EXPECT_TRUE(Near(SpriteMath::ComputeSize(Slice, 2.0f, FVector2(32.0f, 0.0f)), FVector2(32.0f, 16.0f)));
	E_EXPECT_TRUE(Near(SpriteMath::ComputeSize(Slice, 2.0f, FVector2(0.0f, 8.0f)), FVector2(16.0f, 8.0f)));
	E_EXPECT_TRUE(Near(SpriteMath::ComputeSize(Slice, 2.0f, FVector2(10.0f, 20.0f)), FVector2(10.0f, 20.0f)));
}

E_TEST(Sprite_JsonRoundTripAndRepair)
{
	FSpriteAsset Asset;
	Asset.Texture       = "Hero.png";
	Asset.TextureWidth  = 128;
	Asset.TextureHeight = 64;
	Asset.UnitsPerPixel = 0.5f;
	Asset.Filter        = ESpriteFilter::Linear;
	Asset.Slices        = SpriteMath::SliceGrid(128, 64, 32, 32, 0, 0, "Run_");
	Asset.Slices[1].Pivot       = FVector2(0.5f, 0.0f);
	Asset.Slices[2].BorderLeft  = 4;
	Asset.Slices[2].BorderBottom = 6;

	FSpriteAsset             Loaded;
	std::vector<std::string> Warnings;
	E_EXPECT_TRUE(FSpriteAsset::FromJsonString(Asset.ToJsonString(), Loaded, &Warnings));
	E_EXPECT_TRUE(Warnings.empty());
	E_EXPECT_TRUE(Loaded == Asset);
	E_EXPECT_EQ(Loaded.FindSlice("Run_2"), 2);
	E_EXPECT_EQ(Loaded.FindSlice(""), 0); // 빈 이름 = 첫 슬라이스
	E_EXPECT_EQ(Loaded.FindSlice("Missing"), -1);
	E_EXPECT_TRUE(Loaded.Slices[2].HasBorder() && !Loaded.Slices[0].HasBorder());

	// 고쳐 읽기: 이름 중복, 음수 크기, 텍스처 밖, 모르는 필터, 0 이하 단위
	FSpriteAsset Repaired;
	Warnings.clear();
	E_EXPECT_TRUE(FSpriteAsset::FromJsonString(
		R"({"Texture": "A.png", "TextureWidth": 32, "TextureHeight": 32, "UnitsPerPixel": 0, "Filter": "Blurry",
		    "Slices": [{"Name": "A", "W": 16, "H": 16}, {"Name": "A", "W": -1, "H": 4}, {"Name": "B", "X": 24, "W": 16, "H": 16}]})",
		Repaired, &Warnings));
	E_EXPECT_TRUE(Warnings.size() >= 4u);
	E_EXPECT_NEAR(Repaired.UnitsPerPixel, 1.0f, Tolerance);
	E_EXPECT_TRUE(Repaired.Filter == ESpriteFilter::Point);
	E_EXPECT_EQ(Repaired.Slices[1].W, 0);
	E_EXPECT_FALSE(FSpriteAsset::FromJsonString("[1, 2]", Repaired));
}

// ---- 플립북 ------------------------------------------------------------------------------------------------------------

E_TEST(Flipbook_EvaluateLoopOncePingPong)
{
	const auto Frame = [](float Time, EFlipbookLoopMode Mode) { return FlipbookMath::Evaluate(Time, TestDurations, Mode).Frame; };
	E_EXPECT_EQ(Frame(0.05f, EFlipbookLoopMode::Loop), 0);
	E_EXPECT_EQ(Frame(0.1f, EFlipbookLoopMode::Loop), 1); // 경계는 다음 프레임
	E_EXPECT_EQ(Frame(0.35f, EFlipbookLoopMode::Loop), 2);
	E_EXPECT_EQ(Frame(0.65f, EFlipbookLoopMode::Loop), 0);
	E_EXPECT_EQ(Frame(-0.05f, EFlipbookLoopMode::Loop), 2); // 음수도 감싼다 (0.55)

	const FFlipbookSample End = FlipbookMath::Evaluate(0.6f, TestDurations, EFlipbookLoopMode::Once);
	E_EXPECT_EQ(End.Frame, 2);
	E_EXPECT_TRUE(End.bFinished);
	E_EXPECT_FALSE(FlipbookMath::Evaluate(0.59f, TestDurations, EFlipbookLoopMode::Once).bFinished);
	E_EXPECT_EQ(Frame(-1.0f, EFlipbookLoopMode::Once), 0);

	// PingPong 주기 = 2 × 0.6 - 0.1 - 0.3 = 0.8, 순서 0 1 2 1
	E_EXPECT_NEAR(FlipbookMath::GetCycleDuration(TestDurations, EFlipbookLoopMode::PingPong), 0.8f, Tolerance);
	E_EXPECT_EQ(Frame(0.65f, EFlipbookLoopMode::PingPong), 1);
	E_EXPECT_EQ(Frame(0.85f, EFlipbookLoopMode::PingPong), 0);
	E_EXPECT_EQ(FlipbookMath::Evaluate(1.0f, {}, EFlipbookLoopMode::Loop).Frame, -1);

	E_EXPECT_NEAR(FlipbookMath::WrapTime(1.35f, TestDurations, EFlipbookLoopMode::Loop), 0.15f, Tolerance);
	E_EXPECT_NEAR(FlipbookMath::WrapTime(1.35f, TestDurations, EFlipbookLoopMode::Once), 1.35f, Tolerance);
}

E_TEST(Flipbook_EventsForwardLoopAndStart)
{
	E_EXPECT_TRUE(Collect(0.05f, 0.35f, EFlipbookLoopMode::Loop) == (std::vector<int32>{ 1, 2 }));
	E_EXPECT_TRUE(Collect(0.5f, 0.75f, EFlipbookLoopMode::Loop) == (std::vector<int32>{ 0, 1 })); // 루프 경계 넘어 A, B
	E_EXPECT_TRUE(Collect(0.12f, 0.15f, EFlipbookLoopMode::Loop).empty());                         // 같은 프레임 안
	// 재생 시작: 시작 시각이 속한 프레임 이벤트 먼저, 경계는 엄격 (0에서 시작해도 A가 두 번 나오지 않는다)
	E_EXPECT_TRUE(Collect(0.0f, 0.15f, EFlipbookLoopMode::Loop, true) == (std::vector<int32>{ 0, 1 }));
	E_EXPECT_TRUE(Collect(0.05f, 0.05f, EFlipbookLoopMode::Loop, true) == (std::vector<int32>{ 0 }));
	E_EXPECT_TRUE(Collect(0.05f, 0.05f, EFlipbookLoopMode::Loop, false).empty());
}

E_TEST(Flipbook_EventsLargeDeltaCollapsesCycles)
{
	// 0.05 → 2.15 (3.5주기): 앞 2주기는 건너뛰고 (1.25, 2.15]만 → B C A B C — 같은 이벤트는 최대 2번
	E_EXPECT_TRUE(Collect(0.05f, 2.15f, EFlipbookLoopMode::Loop) == (std::vector<int32>{ 1, 2, 0, 1, 2 }));
	// 한 주기 조금 넘게: 건너뛰지 않음
	E_EXPECT_TRUE(Collect(0.05f, 0.75f, EFlipbookLoopMode::Loop) == (std::vector<int32>{ 1, 2, 0, 1 }));
}

E_TEST(Flipbook_EventsReverse)
{
	// 역재생은 프레임 끝으로 들어간다: 0.35(프레임 2) → 0.05 = 1 끝(0.3) B, 0 끝(0.1) A
	E_EXPECT_TRUE(Collect(0.35f, 0.05f, EFlipbookLoopMode::Loop) == (std::vector<int32>{ 1, 0 }));
	// 루프 경계 역방향: 0.05 → -0.1 = 앞 주기 프레임 2의 끝(0.0)으로 들어감
	E_EXPECT_TRUE(Collect(0.05f, -0.1f, EFlipbookLoopMode::Loop) == (std::vector<int32>{ 2 }));
	// PingPong 역방향: 0.75(뒤로 가는 프레임 1) → 0.05 = C, B, A
	E_EXPECT_TRUE(Collect(0.75f, 0.05f, EFlipbookLoopMode::PingPong) == (std::vector<int32>{ 2, 1, 0 }));
}

E_TEST(Flipbook_EventsOnceAndPingPong)
{
	// Once: 끝(0.6)을 넘어가도 끝은 이벤트가 아니다
	E_EXPECT_TRUE(Collect(0.5f, 1.0f, EFlipbookLoopMode::Once).empty());
	E_EXPECT_TRUE(Collect(0.05f, 5.0f, EFlipbookLoopMode::Once) == (std::vector<int32>{ 1, 2 })); // 큰 dt도 한 번만
	// 끝에서 역재생 시작: 마지막 프레임(C) → 1의 끝으로 B
	E_EXPECT_TRUE(Collect(0.6f, 0.25f, EFlipbookLoopMode::Once, true) == (std::vector<int32>{ 2, 1 }));
	// PingPong 정방향: 0 1 2 1 순서 → (0.05, 0.75]에 B, C, B
	E_EXPECT_TRUE(Collect(0.05f, 0.75f, EFlipbookLoopMode::PingPong) == (std::vector<int32>{ 1, 2, 1 }));
}

E_TEST(Flipbook_JsonRoundTripAndTimeline)
{
	FFlipbookAsset Asset;
	Asset.Sprite = "Hero.esprite";
	Asset.Fps    = 8.0f;
	Asset.Loop   = EFlipbookLoopMode::PingPong;
	Asset.Frames = { { "Run_0", 0.0f, 1.0f }, { "Run_1", 0.25f, 1.0f }, { "Run_2", 0.0f, 2.0f } };
	Asset.Events = { { 1, "Footstep" } };
	Asset.RebuildTimeline();
	E_EXPECT_NEAR(Asset.FrameDurations[0], 0.125f, Tolerance);
	E_EXPECT_NEAR(Asset.FrameDurations[1], 0.25f, Tolerance);
	E_EXPECT_NEAR(Asset.FrameDurations[2], 0.25f, Tolerance);
	E_EXPECT_NEAR(Asset.TotalDuration, 0.625f, Tolerance);

	FFlipbookAsset           Loaded;
	std::vector<std::string> Warnings;
	E_EXPECT_TRUE(FFlipbookAsset::FromJsonString(Asset.ToJsonString(), Loaded, &Warnings));
	E_EXPECT_TRUE(Warnings.empty());
	E_EXPECT_TRUE(Loaded == Asset);
	E_EXPECT_TRUE(Loaded.FrameDurations == Asset.FrameDurations);

	// 범위 밖 이벤트·모르는 Loop는 경고
	Warnings.clear();
	E_EXPECT_TRUE(FFlipbookAsset::FromJsonString(R"({"Sprite": "A.esprite", "Loop": "Bounce", "Frames": [{"Slice": "A"}], "Events": [{"Frame": 3, "Name": "E"}]})",
	                                             Loaded, &Warnings));
	E_EXPECT_EQ(Warnings.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Loaded.Loop == EFlipbookLoopMode::Loop);
}

// ---- 타일셋 ------------------------------------------------------------------------------------------------------------

E_TEST(Tileset_GridRectsAndJsonRoundTrip)
{
	FTilesetAsset Tileset;
	Tileset.Texture       = "Tiles.png";
	Tileset.TextureWidth  = 72; // 16px 4칸 + 여백 1 + 간격 2
	Tileset.TextureHeight = 36;
	Tileset.Margin        = 1;
	Tileset.Spacing       = 2;
	E_EXPECT_EQ(Tileset.GetColumns(), 4);
	E_EXPECT_EQ(Tileset.GetRows(), 2);
	const FSpriteSlice Rect = Tileset.GetTileRect(5);
	E_EXPECT_EQ(Rect.X, 19);
	E_EXPECT_EQ(Rect.Y, 19);
	E_EXPECT_EQ(Tileset.GetTileRect(8).W, 0); // 범위 밖

	FTileDefinition Slope;
	Slope.Id        = 3;
	Slope.Collision = ETileCollision::Polygon;
	Slope.Points    = { FVector2(0.0f, 16.0f), FVector2(16.0f, 16.0f), FVector2(16.0f, 0.0f) };
	Slope.Tags      = { "Slope" };
	FTileDefinition Water;
	Water.Id        = 4;
	Water.Animation = { { 4, 0.4f }, { 5, 0.4f } };
	FTileDefinition Platform;
	Platform.Id        = 6;
	Platform.Collision = ETileCollision::Full;
	Platform.bOneWay   = true;
	FTileDefinition Plain; // 기본값 → 저장 안 됨
	Plain.Id      = 1;
	Tileset.Tiles = { Platform, Slope, Plain, Water };
	Tileset.Normalize();
	E_EXPECT_EQ(Tileset.Tiles.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Tileset.Tiles[0].Id, 3); // Id 오름차순
	E_EXPECT_TRUE(Tileset.FindTile(1) == nullptr);
	E_EXPECT_TRUE(Tileset.FindTile(4) != nullptr && Tileset.FindTile(4)->Animation.size() == 2u);
	E_EXPECT_TRUE(Tileset.FindTile(3)->HasTag("Slope"));

	FTilesetAsset            Loaded;
	std::vector<std::string> Warnings;
	E_EXPECT_TRUE(FTilesetAsset::FromJsonString(Tileset.ToJsonString(), Loaded, &Warnings));
	E_EXPECT_TRUE(Warnings.empty());
	E_EXPECT_TRUE(Loaded == Tileset);
	const FSpriteUvRect Uv = Loaded.ComputeTileUv(5);
	E_EXPECT_NEAR(Uv.U0, 19.0f / 72.0f, Tolerance);
	E_EXPECT_NEAR(Uv.V1, 35.0f / 36.0f, Tolerance);
}

// ---- 타일맵 데이터 -----------------------------------------------------------------------------------------------------

E_TEST(Tilemap_CellEncodingBits)
{
	// constexpr 함수에 상수를 넣으면 조건식이 상수가 되어 C4127 — 런타임 값으로 확인한다
	std::vector<int32> Ids = { 0, -1, 41 };
	E_EXPECT_EQ(TileCell::Make(Ids[0]), 1u);
	E_EXPECT_EQ(TileCell::Make(Ids[1]), 0u);
	const uint32 Flags = TileCell::FlipXBit | TileCell::Rotate90Bit;
	std::vector<uint32> Cells = { TileCell::Make(Ids[2], Flags), 0u, TileCell::FlipYBit };
	E_EXPECT_EQ(TileCell::GetTileId(Cells[0]), 41);
	E_EXPECT_EQ(TileCell::GetFlags(Cells[0]), Flags);
	E_EXPECT_EQ(TileCell::GetTileId(Cells[1]), -1);
	E_EXPECT_TRUE(TileCell::IsEmpty(Cells[2])); // 플래그만 = 빈칸
}

E_TEST(Tilemap_SetGetChunkBoundariesNegative)
{
	FTilemapData Data;
	Data.Set(0, 0, TileCell::Make(1));
	Data.Set(31, 0, TileCell::Make(2));
	Data.Set(32, 0, TileCell::Make(3));  // 다음 청크
	Data.Set(-1, -1, TileCell::Make(4)); // 청크 (-1, -1)의 (31, 31)
	Data.Set(-33, 5, TileCell::Make(5)); // 청크 (-2, 0)
	E_EXPECT_EQ(Data.GetChunkCount(), static_cast<size_t>(4));
	E_EXPECT_EQ(Data.GetCellCount(), static_cast<size_t>(5));
	E_EXPECT_EQ(TileCell::GetTileId(Data.Get(32, 0)), 3);
	E_EXPECT_EQ(TileCell::GetTileId(Data.Get(-1, -1)), 4);
	E_EXPECT_EQ(TileCell::GetTileId(Data.Get(-33, 5)), 5);
	E_EXPECT_EQ(Data.Get(-32, 5), 0u);
	E_EXPECT_EQ(Data.Get(1000, -1000), 0u);
	E_EXPECT_TRUE(Data.GetBounds() == (FTileRect{ -33, -1, 32, 5 }));

	Data.Erase(-1, -1); // 청크가 비면 지운다
	E_EXPECT_EQ(Data.GetChunkCount(), static_cast<size_t>(3));
	Data.Set(32, 0, TileCell::FlipXBit); // 플래그만 = 지우기
	E_EXPECT_EQ(Data.GetChunkCount(), static_cast<size_t>(2));
	E_EXPECT_FALSE(FTilemapData().GetBounds().IsValid());
}

E_TEST(Tilemap_FillRectFloodFillAndLine)
{
	FTilemapData Data;
	const uint32 Wall = TileCell::Make(1);
	const uint32 Fill = TileCell::Make(2);
	// 7x7 테두리 (안쪽 5x5 빈칸), 음수 좌표에 걸치게
	Data.FillRect(FTileRect::FromCorners(-3, -3, 3, 3), Wall);
	Data.FillRect(FTileRect::FromCorners(-2, -2, 2, 2), 0);
	E_EXPECT_EQ(Data.GetCellCount(), static_cast<size_t>(24));
	const FTileRect Limit = Data.GetBounds();
	E_EXPECT_EQ(Data.FloodFill(0, 0, Fill, Limit), 25);
	E_EXPECT_EQ(TileCell::GetTileId(Data.Get(2, -2)), 2);
	E_EXPECT_EQ(TileCell::GetTileId(Data.Get(3, 3)), 1); // 벽은 그대로
	E_EXPECT_EQ(Data.FloodFill(0, 0, Fill, Limit), 0);   // 이미 같은 값
	E_EXPECT_EQ(Data.FloodFill(10, 10, Fill, Limit), 0); // 경계 밖 시작

	// 열린 빈 영역: 상한을 넘으면 아무것도 바꾸지 않는다
	const size_t Before = Data.GetCellCount();
	E_EXPECT_EQ(Data.FloodFill(10, 10, Fill, FTileRect::FromCorners(4, 4, 100, 100), 100), -1);
	E_EXPECT_EQ(Data.GetCellCount(), Before);
	// 테두리 바깥 빈칸을 경계 안에서 채움: 9x9 경계 - 7x7 = 32칸
	E_EXPECT_EQ(Data.FloodFill(-4, -4, Fill, FTileRect::FromCorners(-4, -4, 4, 4)), 32);

	// 브레젠험 (0,0)→(5,2): 6칸, 음수 방향 대각선
	const std::vector<FTileCoord> Line = TilemapMath::LineCells(0, 0, 5, 2);
	const std::vector<FTileCoord> Expected = { { 0, 0 }, { 1, 0 }, { 2, 1 }, { 3, 1 }, { 4, 2 }, { 5, 2 } };
	E_EXPECT_TRUE(Line == Expected);
	const std::vector<FTileCoord> Diagonal = TilemapMath::LineCells(0, 0, -3, -3);
	E_EXPECT_EQ(Diagonal.size(), static_cast<size_t>(4));
	E_EXPECT_TRUE(Diagonal.back() == (FTileCoord{ -3, -3 }));
	FTilemapData Lines;
	E_EXPECT_EQ(Lines.DrawLine(2, 7, 2, 7, Wall), 1); // 한 점
}

E_TEST(Tilemap_CopyPasteRegion)
{
	FTilemapData Data;
	Data.Set(0, 0, TileCell::Make(1));
	Data.Set(1, 0, TileCell::Make(2, TileCell::FlipYBit));
	Data.Set(1, 1, TileCell::Make(3));
	const FTilemapRegion Region = Data.CopyRegion(FTileRect::FromCorners(0, 0, 1, 1));
	E_EXPECT_EQ(Region.Width, 2);
	E_EXPECT_EQ(Region.Height, 2);
	E_EXPECT_EQ(Region.Get(0, 1), 0u); // 빈칸
	E_EXPECT_EQ(TileCell::GetFlags(Region.Get(1, 0)), TileCell::FlipYBit);

	FTilemapData Target;
	Target.Set(-10, -9, TileCell::Make(9)); // 영역의 빈칸 (0,1) 자리
	Target.PasteRegion(Region, -10, -10);
	E_EXPECT_EQ(TileCell::GetTileId(Target.Get(-9, -9)), 3);
	E_EXPECT_EQ(TileCell::GetTileId(Target.Get(-10, -9)), 9); // bSkipEmpty: 기존 유지
	Target.PasteRegion(Region, -10, -10, false);
	E_EXPECT_EQ(Target.Get(-10, -9), 0u);
}

E_TEST(Tilemap_EncodeDecodeRoundTripDeterministic)
{
	FTilemapData A;
	A.Set(-40, -3, TileCell::Make(7, TileCell::Rotate90Bit));
	A.FillRect(FTileRect::FromCorners(0, 0, 40, 2), TileCell::Make(1));
	A.Set(100, 100, TileCell::Make(TileCell::MaxTileId, TileCell::FlagMask));
	// 같은 내용을 다른 순서로
	FTilemapData B;
	B.Set(100, 100, TileCell::Make(TileCell::MaxTileId, TileCell::FlagMask));
	for (int32 X = 40; X >= 0; --X)
	{
		for (int32 Y = 2; Y >= 0; --Y)
		{
			B.Set(X, Y, TileCell::Make(1));
		}
	}
	B.Set(-40, -3, TileCell::Make(7, TileCell::Rotate90Bit));
	const std::string Encoded = A.Encode();
	E_EXPECT_FALSE(Encoded.empty());
	E_EXPECT_EQ(Encoded, B.Encode()); // 결정적

	FTilemapData Decoded;
	std::string  Error;
	E_EXPECT_TRUE(FTilemapData::Decode(Encoded, Decoded, &Error));
	E_EXPECT_TRUE(Decoded == A);
	E_EXPECT_EQ(Decoded.GetCellCount(), A.GetCellCount());
	E_EXPECT_EQ(Decoded.Encode(), Encoded);

	E_EXPECT_EQ(FTilemapData().Encode(), std::string()); // 빈 맵 = 빈 문자열
	E_EXPECT_TRUE(FTilemapData::Decode("", Decoded) && Decoded.IsEmpty());
}

E_TEST(Tilemap_DecodeRejectsCorruptInput)
{
	FTilemapData Source;
	Source.FillRect(FTileRect::FromCorners(0, 0, 3, 3), TileCell::Make(2));
	const std::string Valid = Source.Encode();
	std::vector<uint8> Bytes;
	E_EXPECT_TRUE(TerrainIO::DecodeBase64(Valid, Bytes));

	const auto ExpectFail = [](const std::string& Text) {
		FTilemapData Out;
		Out.Set(5, 5, TileCell::Make(1)); // 실패하면 비워져야 한다
		std::string Error;
		E_EXPECT_FALSE(FTilemapData::Decode(Text, Out, &Error));
		E_EXPECT_FALSE(Error.empty());
		E_EXPECT_TRUE(Out.IsEmpty());
	};
	ExpectFail("@@@ not base64 @@@");
	std::vector<uint8> Truncated(Bytes.begin(), Bytes.end() - 3);
	ExpectFail(TerrainIO::EncodeBase64(Truncated.data(), Truncated.size()));
	std::vector<uint8> BadVersion = Bytes;
	BadVersion[0]                 = 99;
	ExpectFail(TerrainIO::EncodeBase64(BadVersion.data(), BadVersion.size()));
	std::vector<uint8> BadRun = Bytes;
	BadRun[13]                = 0; // 첫 RLE 길이(uint16, 오프셋 13) = 0
	BadRun[14]                = 0;
	ExpectFail(TerrainIO::EncodeBase64(BadRun.data(), BadRun.size()));
	std::vector<uint8> HugeCount = Bytes;
	HugeCount[4]                 = 0x7F; // 청크 수 터무니없음
	ExpectFail(TerrainIO::EncodeBase64(HugeCount.data(), HugeCount.size()));
	std::vector<uint8> Trailing = Bytes;
	Trailing.push_back(0);
	ExpectFail(TerrainIO::EncodeBase64(Trailing.data(), Trailing.size()));
}

E_TEST(Tilemap_CellLocalConversion)
{
	const FVector2 CellSize(16.0f, 8.0f);
	E_EXPECT_TRUE(Near(TilemapMath::CellToLocal(-1, 2, CellSize), FVector2(-16.0f, 16.0f)));
	E_EXPECT_TRUE(Near(TilemapMath::CellCenterToLocal(-1, 2, CellSize), FVector2(-8.0f, 20.0f)));
	E_EXPECT_TRUE(TilemapMath::LocalToCell(FVector2(-0.1f, 15.9f), CellSize) == (FTileCoord{ -1, 1 }));
	E_EXPECT_TRUE(TilemapMath::LocalToCell(FVector2(0.0f, 0.0f), CellSize) == (FTileCoord{ 0, 0 }));
	E_EXPECT_TRUE(TilemapMath::LocalToCell(FVector2(16.0f, -8.0f), CellSize) == (FTileCoord{ 1, -1 })); // 경계는 오른쪽/위 셀
	// 타일 변환: 반시계 90° 후 반전
	E_EXPECT_TRUE(Near(TilemapMath::TransformTileUv(FVector2(0.5f, 0.0f), TileCell::Rotate90Bit), FVector2(0.0f, 0.5f)));
	E_EXPECT_TRUE(Near(TilemapMath::TransformTileUv(FVector2(0.5f, 0.0f), TileCell::Rotate90Bit | TileCell::FlipYBit), FVector2(0.0f, -0.5f)));
	E_EXPECT_TRUE(TilemapMath::FlipsWinding(TileCell::FlipXBit) && !TilemapMath::FlipsWinding(TileCell::FlipXBit | TileCell::FlipYBit));
}

// ---- 타일 충돌 ---------------------------------------------------------------------------------------------------------

E_TEST(TilemapCollision_MergesBoxesAndTransformsPolygons)
{
	FTilesetAsset Tileset;
	Tileset.TextureWidth  = 64;
	Tileset.TextureHeight = 32;
	FTileDefinition Solid;
	Solid.Id        = 0;
	Solid.Collision = ETileCollision::Full;
	FTileDefinition Platform;
	Platform.Id        = 1;
	Platform.Collision = ETileCollision::Full;
	Platform.bOneWay   = true;
	FTileDefinition Slope;
	Slope.Id        = 2;
	Slope.Collision = ETileCollision::Polygon;
	Slope.Points    = { FVector2(0.0f, 16.0f), FVector2(16.0f, 16.0f), FVector2(16.0f, 0.0f) }; // 이미지 기준 왼쪽 아래, 오른쪽 아래, 오른쪽 위
	Tileset.Tiles   = { Solid, Platform, Slope };

	FTilemapData Data;
	Data.FillRect(FTileRect::FromCorners(0, 0, 3, 1), TileCell::Make(0)); // 4x2 덩어리
	Data.FillRect(FTileRect::FromCorners(0, 2, 1, 2), TileCell::Make(0)); // 그 위 2칸
	Data.FillRect(FTileRect::FromCorners(5, 0, 6, 0), TileCell::Make(1)); // 원웨이 2칸
	Data.Set(10, 0, TileCell::Make(2));
	Data.Set(12, 0, TileCell::Make(2, TileCell::FlipXBit));
	Data.Set(14, 0, TileCell::Make(2, TileCell::Rotate90Bit));
	Data.Set(16, 0, TileCell::Make(7)); // 정의 없는 타일 = 충돌 없음

	const FTilemapCollisionShapes Shapes = TilemapCollision::BuildShapes(Data, Tileset, FVector2::ZeroVector); // 셀 = 16cm
	E_EXPECT_EQ(Shapes.Boxes.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Shapes.Boxes[0] == (FTileCollisionBox{ FVector2(0.0f, 0.0f), FVector2(64.0f, 32.0f) }));
	E_EXPECT_TRUE(Shapes.Boxes[1] == (FTileCollisionBox{ FVector2(0.0f, 32.0f), FVector2(32.0f, 48.0f) }));
	E_EXPECT_EQ(Shapes.OneWayBoxes.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Shapes.OneWayBoxes[0] == (FTileCollisionBox{ FVector2(80.0f, 0.0f), FVector2(112.0f, 16.0f) }));

	E_EXPECT_EQ(Shapes.Polygons.size(), static_cast<size_t>(3));
	const std::vector<FVector2> Plain   = { FVector2(160.0f, 0.0f), FVector2(176.0f, 0.0f), FVector2(176.0f, 16.0f) };
	const std::vector<FVector2> FlipX   = { FVector2(192.0f, 16.0f), FVector2(192.0f, 0.0f), FVector2(208.0f, 0.0f) }; // 거울 + 순서 뒤집음
	const std::vector<FVector2> Rotated = { FVector2(240.0f, 0.0f), FVector2(240.0f, 16.0f), FVector2(224.0f, 16.0f) };
	const auto Same = [](const std::vector<FVector2>& A, const std::vector<FVector2>& B) {
		if (A.size() != B.size())
		{
			return false;
		}
		for (size_t Index = 0; Index < A.size(); ++Index)
		{
			if (!Near(A[Index], B[Index], 1.0e-3f))
			{
				return false;
			}
		}
		return true;
	};
	E_EXPECT_TRUE(Same(Shapes.Polygons[0].Points, Plain));
	E_EXPECT_TRUE(Same(Shapes.Polygons[1].Points, FlipX));
	E_EXPECT_TRUE(Same(Shapes.Polygons[2].Points, Rotated));
	E_EXPECT_TRUE(Shapes.OneWayPolygons.empty());

	// 셀 크기 지정: 상자도 그 크기로
	const FTilemapCollisionShapes Scaled = TilemapCollision::BuildShapes(Data, Tileset, FVector2(100.0f, 50.0f));
	E_EXPECT_TRUE(Scaled.Boxes[0] == (FTileCollisionBox{ FVector2(0.0f, 0.0f), FVector2(400.0f, 100.0f) }));
}

// 외곽선 (TilemapCollision::TraceOutlines): 영역이 진행 방향 왼쪽 (바깥 반시계, 구멍 시계), 공선 점 병합, (Y, X) 최소 꼭짓점부터, 결정적 순서
namespace
{
	using FLoop = std::vector<FTileCoord>;

	std::vector<FTileCoord> RectCells(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
	{
		std::vector<FTileCoord> Cells;
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				Cells.push_back({ X, Y });
			}
		}
		return Cells;
	}

	void RemoveCells(std::vector<FTileCoord>& Cells, std::initializer_list<FTileCoord> Remove)
	{
		std::erase_if(Cells, [&](const FTileCoord& Cell) { return std::find(Remove.begin(), Remove.end(), Cell) != Remove.end(); });
	}
} // namespace

E_TEST(TilemapCollision_OutlineRectangleAndL)
{
	const std::vector<FLoop> Rect = TilemapCollision::TraceOutlines(RectCells(0, 0, 2, 1));
	E_EXPECT_EQ(Rect.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Rect[0] == (FLoop{ { 0, 0 }, { 3, 0 }, { 3, 2 }, { 0, 2 } })); // 반시계, 공선 점 없음

	// L자 (중복 칸 허용)
	const std::vector<FLoop> L = TilemapCollision::TraceOutlines({ { 0, 2 }, { 0, 0 }, { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 0 } });
	E_EXPECT_EQ(L.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(L[0] == (FLoop{ { 0, 0 }, { 3, 0 }, { 3, 1 }, { 1, 1 }, { 1, 3 }, { 0, 3 } }));
	E_EXPECT_TRUE(TilemapCollision::TraceOutlines({}).empty());
}

E_TEST(TilemapCollision_OutlineHoleIslandsAndDiagonals)
{
	// 가운데 빈 3x3 고리: 바깥(반시계) + 구멍(시계)
	std::vector<FTileCoord> Ring = RectCells(0, 0, 2, 2);
	RemoveCells(Ring, { { 1, 1 } });
	const std::vector<FLoop> RingLoops = TilemapCollision::TraceOutlines(Ring);
	E_EXPECT_EQ(RingLoops.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(RingLoops[0] == (FLoop{ { 0, 0 }, { 3, 0 }, { 3, 3 }, { 0, 3 } }));
	E_EXPECT_TRUE(RingLoops[1] == (FLoop{ { 1, 1 }, { 1, 2 }, { 2, 2 }, { 2, 1 } }));

	// 대각으로만 닿은 칸 = 꼭짓점 하나를 공유하는 두 고리
	const std::vector<FLoop> Diagonal = TilemapCollision::TraceOutlines({ { 1, 1 }, { 0, 0 } });
	E_EXPECT_EQ(Diagonal.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Diagonal[0] == (FLoop{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }));
	E_EXPECT_TRUE(Diagonal[1] == (FLoop{ { 1, 1 }, { 2, 1 }, { 2, 2 }, { 1, 2 } }));

	// 대각으로 닿은 구멍 두 개도 나뉜다 (자기 접촉 고리 없음)
	std::vector<FTileCoord> Holes = RectCells(0, 0, 3, 3);
	RemoveCells(Holes, { { 1, 1 }, { 2, 2 } });
	const std::vector<FLoop> HoleLoops = TilemapCollision::TraceOutlines(Holes);
	E_EXPECT_EQ(HoleLoops.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(HoleLoops[0] == (FLoop{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } }));
	E_EXPECT_TRUE(HoleLoops[1] == (FLoop{ { 1, 1 }, { 1, 2 }, { 2, 2 }, { 2, 1 } }));
	E_EXPECT_TRUE(HoleLoops[2] == (FLoop{ { 2, 2 }, { 2, 3 }, { 3, 3 }, { 3, 2 } }));

	// 분리된 섬 (음수 좌표 포함): (Y, X) 순, 입력 순서와 무관
	std::vector<FTileCoord> Islands = RectCells(5, 0, 6, 0);
	Islands.push_back({ -3, 0 });
	Islands.push_back({ 0, -2 });
	const std::vector<FLoop> IslandLoops = TilemapCollision::TraceOutlines(Islands);
	std::reverse(Islands.begin(), Islands.end());
	E_EXPECT_TRUE(TilemapCollision::TraceOutlines(Islands) == IslandLoops);
	E_EXPECT_EQ(IslandLoops.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(IslandLoops[0] == (FLoop{ { 0, -2 }, { 1, -2 }, { 1, -1 }, { 0, -1 } }));
	E_EXPECT_TRUE(IslandLoops[1] == (FLoop{ { -3, 0 }, { -2, 0 }, { -2, 1 }, { -3, 1 } }));
	E_EXPECT_TRUE(IslandLoops[2] == (FLoop{ { 5, 0 }, { 7, 0 }, { 7, 1 }, { 5, 1 } }));
}

// BuildShapes의 외곽선(cm, 구멍 표시)·원웨이 윗변 선분 (같은 행 이어 붙임, 위 칸이 원웨이면 그 칸은 윗변 없음)
E_TEST(TilemapCollision_OutlinesAndOneWaySegmentsInShapes)
{
	FTilesetAsset Tileset;
	Tileset.TextureWidth  = 32;
	Tileset.TextureHeight = 16;
	FTileDefinition Solid;
	Solid.Id        = 0;
	Solid.Collision = ETileCollision::Full;
	FTileDefinition Platform;
	Platform.Id        = 1;
	Platform.Collision = ETileCollision::Full;
	Platform.bOneWay   = true;
	Tileset.Tiles      = { Solid, Platform };

	FTilemapData Data;
	Data.FillRect(FTileRect::FromCorners(0, 0, 2, 2), TileCell::Make(0));
	Data.Erase(1, 1);
	Data.FillRect(FTileRect::FromCorners(5, 0, 7, 0), TileCell::Make(1)); // 원웨이 3칸
	Data.Set(6, 1, TileCell::Make(1));                                    // 가운데 위에 하나 더
	const FTilemapCollisionShapes Shapes = TilemapCollision::BuildShapes(Data, Tileset, FVector2(100.0f, 50.0f));
	E_EXPECT_EQ(Shapes.Outlines.size(), static_cast<size_t>(2));
	E_EXPECT_FALSE(Shapes.Outlines[0].bHole);
	E_EXPECT_TRUE(Shapes.Outlines[0].Points ==
	              (std::vector<FVector2>{ FVector2(0.0f, 0.0f), FVector2(300.0f, 0.0f), FVector2(300.0f, 150.0f), FVector2(0.0f, 150.0f) }));
	E_EXPECT_TRUE(Shapes.Outlines[1].bHole);
	E_EXPECT_TRUE(Shapes.Outlines[1].Points ==
	              (std::vector<FVector2>{ FVector2(100.0f, 50.0f), FVector2(100.0f, 100.0f), FVector2(200.0f, 100.0f), FVector2(200.0f, 50.0f) }));
	// 원웨이: (5,0)·(7,0) 윗변은 따로 (가운데 (6,0) 위는 원웨이), (6,1) 윗변. Start → End = -X
	E_EXPECT_EQ(Shapes.OneWaySegments.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Shapes.OneWaySegments[0] == (FTileCollisionSegment{ FVector2(600.0f, 50.0f), FVector2(500.0f, 50.0f) }));
	E_EXPECT_TRUE(Shapes.OneWaySegments[1] == (FTileCollisionSegment{ FVector2(800.0f, 50.0f), FVector2(700.0f, 50.0f) }));
	E_EXPECT_TRUE(Shapes.OneWaySegments[2] == (FTileCollisionSegment{ FVector2(700.0f, 100.0f), FVector2(600.0f, 100.0f) }));
	E_EXPECT_FALSE(Shapes.Boxes.empty()); // 병합 상자도 그대로 (다른 용도)
}

// ---- 라이브러리 / 샘플 에셋 / 컴포넌트 ---------------------------------------------------------------------------------

E_TEST(Sprite2D_LoadsSampleAssets)
{
	if (!fs::exists(SampleDirectory() / L"SampleAtlas.esprite"))
	{
		E_LOG(LogCore, Warning, "샘플 2D 에셋 없음 — 테스트 건너뜀");
		return;
	}
	FSprite2DLibrary& Library = FSprite2DLibrary::Get();
	const auto        Sprite  = Library.LoadSprite(SamplePath(L"SampleAtlas.esprite"));
	E_EXPECT_TRUE(Sprite != nullptr);
	if (Sprite == nullptr)
	{
		return;
	}
	E_EXPECT_EQ(Sprite->Slices.size(), static_cast<size_t>(9));
	E_EXPECT_TRUE(Sprite->Filter == ESpriteFilter::Point);
	// 샘플 아이콘 8개 배치는 SliceGrid(144, 72, 34, 34, 1, 2)와 같다 (그 아래 줄 = 9-슬라이스 패널 — Phase 56-4c)
	const std::vector<FSpriteSlice> Grid = SpriteMath::SliceGrid(Sprite->TextureWidth, 72, 34, 34, 1, 2, "");
	E_EXPECT_EQ(Grid.size(), static_cast<size_t>(8));
	E_EXPECT_TRUE(Sprite->Slices[8].Name == "Panel" && Sprite->Slices[8].HasBorder());
	E_EXPECT_EQ(Grid[4].X, Sprite->Slices[4].X);
	E_EXPECT_EQ(Grid[4].Y, Sprite->Slices[4].Y);
	E_EXPECT_TRUE(Library.LoadSprite(SamplePath(L"SampleAtlas.esprite")) == Sprite); // 캐시
	// 텍스처 상대 경로 → 실제 파일
	const std::string Texture = FSprite2DLibrary::ResolveReference(SamplePath(L"SampleAtlas.esprite"), Sprite->Texture);
	E_EXPECT_TRUE(fs::exists(Library.ResolvePath(Texture)));

	const auto Flipbook = Library.LoadFlipbook(SamplePath(L"SampleGems.eflipbook"));
	E_EXPECT_TRUE(Flipbook != nullptr && Flipbook->Frames.size() == 3u && Flipbook->Loop == EFlipbookLoopMode::PingPong);
	const auto Tileset = Library.LoadTileset(SamplePath(L"SampleTiles.etileset"));
	E_EXPECT_TRUE(Tileset != nullptr);
	if (Tileset != nullptr)
	{
		E_EXPECT_EQ(Tileset->GetTileCount(), 8);
		E_EXPECT_TRUE(Tileset->FindTile(6) != nullptr && Tileset->FindTile(6)->bOneWay);
		E_EXPECT_TRUE(Tileset->FindTile(4) != nullptr && Tileset->FindTile(4)->HasTag("Water"));
	}
	E_EXPECT_TRUE(Library.LoadSprite(SamplePath(L"Missing.esprite")) == nullptr); // 없음 = nullptr (경고 한 번)
}

E_TEST(Sprite2D_SaveInvalidatesAndBumpsGeneration)
{
	const fs::path Directory = FTestRegistry::GetTempDirectory() / "ProjectE_Sprite2DTests";
	std::error_code ErrorCode;
	fs::remove_all(Directory, ErrorCode);
	const std::string Path = (Directory / "Saved.esprite").generic_string();

	FSprite2DLibrary& Library = FSprite2DLibrary::Get();
	FSpriteAsset      Asset;
	Asset.Texture = "A.png";
	Asset.Slices  = SpriteMath::SliceGrid(32, 16, 16, 16, 0, 0, "S");
	E_EXPECT_TRUE(Library.SaveSprite(Path, Asset));
	const auto   First      = Library.LoadSprite(Path);
	const uint32 Generation = Library.GetGeneration();
	E_EXPECT_TRUE(First != nullptr && First->Slices.size() == 2u);

	Asset.Slices.pop_back();
	E_EXPECT_TRUE(Library.SaveSprite(Path, Asset));
	E_EXPECT_TRUE(Library.GetGeneration() != Generation);
	const auto Second = Library.LoadSprite(Path);
	E_EXPECT_TRUE(Second != nullptr && Second->Slices.size() == 1u);
	E_EXPECT_EQ(First->Slices.size(), static_cast<size_t>(2)); // 받은 객체는 불변
	fs::remove_all(Directory, ErrorCode);
}

E_TEST(Sprite2D_FlipbookSystemDrivesSprite)
{
	if (!fs::exists(SampleDirectory() / L"SampleGems.eflipbook"))
	{
		return;
	}
	FScene     Scene;
	FEntity    Entity   = Scene.CreateEntity("Gem");
	FRegistry& Registry = Scene.GetRegistry();
	Registry.Emplace<FSpriteComponent>(Entity).Sprite      = SamplePath(L"SampleAtlas.esprite");
	FFlipbookComponent& Flipbook                           = Registry.Emplace<FFlipbookComponent>(Entity);
	Flipbook.Flipbook                                      = SamplePath(L"SampleGems.eflipbook");

	// 프레임 길이 0.125 / 0.25 / 0.25 (PingPong, Sparkle = 프레임 1)
	FFlipbookSystem::Update(Scene, 0.0f);
	E_EXPECT_EQ(FFlipbookSystem::GetFrame(Scene, Entity), 0);
	E_EXPECT_TRUE(Registry.Get<FFlipbookComponent>(Entity).Runtime.PendingEvents.empty()); // 프레임 0에는 이벤트 없음
	FSpriteComponent& Sprite = Registry.Get<FSpriteComponent>(Entity);
	Sprite2DRuntime::FSpriteDisplay Display = Sprite2DRuntime::ResolveSprite(Sprite);
	E_EXPECT_TRUE(Display.Asset != nullptr && Display.SliceIndex == Display.Asset->FindSlice("Coin"));

	FFlipbookSystem::Update(Scene, 0.13f);
	E_EXPECT_EQ(FFlipbookSystem::GetFrame(Scene, Entity), 1);
	const auto& Events = Registry.Get<FFlipbookComponent>(Entity).Runtime.PendingEvents;
	E_EXPECT_TRUE(Events.size() == 1u && Events[0].Name == "Sparkle" && Events[0].Frame == 1);
	Display = Sprite2DRuntime::ResolveSprite(Sprite);
	E_EXPECT_TRUE(Display.Asset != nullptr && Display.SliceIndex == Display.Asset->FindSlice("Ruby"));

	FFlipbookSystem::Update(Scene, 0.01f); // 같은 프레임 — 이벤트는 매 갱신 비운다
	E_EXPECT_TRUE(Registry.Get<FFlipbookComponent>(Entity).Runtime.PendingEvents.empty());

	// 플립북을 떼면 스프라이트 자체 슬라이스로
	Registry.Get<FSpriteComponent>(Entity).Slice = "Fire";
	Registry.Get<FFlipbookComponent>(Entity).Flipbook.clear();
	FFlipbookSystem::Update(Scene, 0.01f);
	Display = Sprite2DRuntime::ResolveSprite(Registry.Get<FSpriteComponent>(Entity));
	E_EXPECT_TRUE(Display.Asset != nullptr && Display.SliceIndex == Display.Asset->FindSlice("Fire"));
}

E_TEST(Sprite2D_ComponentsReflectAndSerialize)
{
	FScene               Scene;
	const FTypeRegistry& Registry = FTypeRegistry::Get();
	const FTypeInfo*     Sprite   = Registry.Find<FSpriteComponent>();
	E_EXPECT_TRUE(Sprite != nullptr && Sprite->bIsComponent);
	if (Sprite == nullptr)
	{
		return;
	}
	E_EXPECT_TRUE(Sprite->FindProperty("SortingLayer")->StringOptions != nullptr);
	{
		// 슬라이스 콤보: 그 컴포넌트가 고른 .esprite의 슬라이스 이름 (인스턴스별 공급자). 미선택·없는 파일 = 빈 목록 (일반 문자열 칸)
		const fs::path Directory = FTestRegistry::GetTempDirectory() / "ProjectE_Sprite2DSliceOptions";
		fs::create_directories(Directory);
		FSpriteAsset Atlas;
		Atlas.Texture       = "A.png";
		Atlas.TextureWidth  = 32;
		Atlas.TextureHeight = 16;
		Atlas.Slices        = SpriteMath::SliceGrid(32, 16, 16, 16, 0, 0, "Gem");
		Atlas.Slices[1].Name = "Coin";
		const std::string AtlasPath = (Directory / "Options.esprite").generic_string();
		E_EXPECT_TRUE(FSprite2DLibrary::Get().SaveSprite(AtlasPath, Atlas));
		const FPropertyInfo* Slice = Sprite->FindProperty("Slice");
		E_EXPECT_TRUE(Slice->StringOptionsFor != nullptr);
		FSpriteComponent Component;
		E_EXPECT_TRUE(Slice->GetStringOptions(&Component).empty());
		Component.Sprite = AtlasPath;
		E_EXPECT_TRUE(Slice->GetStringOptions(&Component) == (std::vector<std::string>{ Atlas.Slices[0].Name, "Coin" }));
		Component.Sprite = (Directory / "Missing.esprite").generic_string();
		E_EXPECT_TRUE(Slice->GetStringOptions(&Component).empty());
	}
	E_EXPECT_EQ(Sprite->FindProperty("Sprite")->AssetFilter, std::string(".esprite"));
	E_EXPECT_TRUE(Sprite->FindProperty("Color")->HasFlag(PF_Color));
	E_EXPECT_TRUE(Sprite->FindProperty("Runtime") == nullptr);
	const FTypeInfo* Tilemap = Registry.Find<FTilemapComponent>();
	E_EXPECT_TRUE(Tilemap != nullptr && Tilemap->FindProperty("TileData")->HasFlag(PF_Hidden));
	E_EXPECT_TRUE(Registry.Find<FFlipbookComponent>() != nullptr);

	// 씬 JSON 왕복: 타일 데이터 문자열과 스프라이트 값 유지
	FEntity            Entity = Scene.CreateEntity("Ground");
	FTilemapComponent& Layer  = Scene.GetRegistry().Emplace<FTilemapComponent>(Entity);
	Layer.Tileset             = "Sprites/Samples/SampleTiles.etileset";
	Layer.SortingLayer        = "Background";
	Layer.Runtime.Data.FillRect(FTileRect::FromCorners(-2, 0, 5, 1), TileCell::Make(1));
	Sprite2DRuntime::CommitTilemapData(Layer);
	FSpriteComponent& Hero = Scene.GetRegistry().Emplace<FSpriteComponent>(Scene.CreateEntity("Hero"));
	Hero.Slice             = "Coin";
	Hero.bFlipX            = true;
	Hero.OrderInLayer      = 3;

	FScene Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, FSceneSerializer::ToJsonString(Scene)));
	bool bFoundTilemap = false;
	bool bFoundSprite  = false;
	Loaded.GetRegistry().View<FTilemapComponent>().Each([&](FEntity, FTilemapComponent& Component) {
		bFoundTilemap = true;
		E_EXPECT_EQ(Component.SortingLayer, std::string("Background"));
		E_EXPECT_EQ(Sprite2DRuntime::GetTilemapData(Component).GetCellCount(), static_cast<size_t>(16));
	});
	Loaded.GetRegistry().View<FSpriteComponent>().Each([&](FEntity, FSpriteComponent& Component) {
		bFoundSprite = true;
		E_EXPECT_TRUE(Component.bFlipX && Component.OrderInLayer == 3 && Component.Slice == "Coin");
	});
	E_EXPECT_TRUE(bFoundTilemap && bFoundSprite);

	// 손상된 타일 데이터: 빈 맵 (오류 로그 한 번)
	FTilemapComponent Broken;
	Broken.TileData = "!!!";
	E_EXPECT_TRUE(Sprite2DRuntime::GetTilemapData(Broken).IsEmpty());
}

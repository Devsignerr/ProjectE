#include "Core/Testing/TestFramework.h"
#include "Editor/AssetEditors/Sprite2DAssetCreation.h"
#include "Editor/AssetEditors/Sprite2DEditing.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <cmath>
#include <string>
#include <tuple>
#include <vector>

// Phase 56-5b: 2D 에셋 편집기 순수 편집 연산 (Sprite2DEditing.h 머리 주석이 기준)

using namespace Sprite2DEditing;

namespace
{
	bool RectIs(const FPixelRect& Rect, int32 X, int32 Y, int32 W, int32 H) { return Rect == FPixelRect{ X, Y, W, H }; }

	FSpriteAsset MakeAtlas()
	{
		FSpriteAsset Asset;
		Asset.TextureWidth  = 64;
		Asset.TextureHeight = 32;
		for (const char* Name : { "A", "B", "C" })
		{
			FSpriteSlice Slice;
			Slice.Name = Name;
			Slice.W    = 8;
			Slice.H    = 8;
			Asset.Slices.push_back(Slice);
		}
		return Asset;
	}

	FFlipbookAsset MakeFlipbook(int32 Count)
	{
		FFlipbookAsset Asset;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FFlipbookFrame Frame;
			Frame.Slice = "F" + std::to_string(Index);
			Asset.Frames.push_back(Frame);
		}
		Asset.RebuildTimeline();
		return Asset;
	}

	std::vector<std::string> SliceOrder(const FFlipbookAsset& Asset)
	{
		std::vector<std::string> Names;
		for (const FFlipbookFrame& Frame : Asset.Frames)
		{
			Names.push_back(Frame.Slice);
		}
		return Names;
	}
} // namespace

E_TEST(Sprite2DEditing_DragRectSnapsAndNormalizes)
{
	// 반올림 스냅 + 거꾸로 끌어도 정규화
	E_EXPECT_TRUE(RectIs(MakeRectFromDrag(FVector2(1.4f, 2.6f), FVector2(9.6f, 7.2f), 64, 32), 1, 3, 9, 4));
	E_EXPECT_TRUE(RectIs(MakeRectFromDrag(FVector2(9.6f, 7.2f), FVector2(1.4f, 2.6f), 64, 32), 1, 3, 9, 4));
	// 텍스처 밖은 잘림
	E_EXPECT_TRUE(RectIs(MakeRectFromDrag(FVector2(-5.0f, -5.0f), FVector2(70.0f, 40.0f), 64, 32), 0, 0, 64, 32));
	// 너무 작은 드래그 = 크기 0
	E_EXPECT_EQ(MakeRectFromDrag(FVector2(3.2f, 3.2f), FVector2(3.4f, 9.0f), 64, 32).W, 0);
	// 텍스처 크기 0 = 자르지 않음
	E_EXPECT_TRUE(RectIs(MakeRectFromDrag(FVector2(-2.0f, 0.0f), FVector2(3.0f, 4.0f), 0, 0), -2, 0, 5, 4));
	// 정규화: 모서리 교환 + 최소 1px
	E_EXPECT_TRUE(RectIs(NormalizeRect(10, 8, 4, 2), 4, 2, 6, 6));
	E_EXPECT_TRUE(RectIs(NormalizeRect(5, 5, 5, 5), 5, 5, 1, 1));
}

E_TEST(Sprite2DEditing_ResizeHandlesFlipAndClamp)
{
	const FPixelRect Rect{ 10, 10, 8, 6 };
	// 오른쪽 아래 핸들
	E_EXPECT_TRUE(RectIs(ApplyRectDrag(Rect, ERectHandle::BottomRight, FVector2(2.4f, 1.6f), 64, 32), 10, 10, 10, 8));
	// 왼쪽 핸들이 오른쪽 모서리를 넘으면 뒤집힘
	E_EXPECT_TRUE(RectIs(ApplyRectDrag(Rect, ERectHandle::Left, FVector2(12.0f, 0.0f), 64, 32), 18, 10, 4, 6));
	// 위 핸들을 아래 모서리에 딱 맞추면 최소 1px
	E_EXPECT_TRUE(RectIs(ApplyRectDrag(Rect, ERectHandle::Top, FVector2(0.0f, 6.0f), 64, 32), 10, 16, 8, 1));
	// 이동은 크기 유지 + 텍스처 안
	E_EXPECT_TRUE(RectIs(ApplyRectDrag(Rect, ERectHandle::Move, FVector2(100.0f, -100.0f), 64, 32), 56, 0, 8, 6));
	// 크기 조절도 텍스처 안으로
	E_EXPECT_TRUE(RectIs(ApplyRectDrag(Rect, ERectHandle::TopLeft, FVector2(-50.0f, -50.0f), 64, 32), 0, 0, 18, 16));
}

E_TEST(Sprite2DEditing_HitTestHandles)
{
	const FPixelRect Rect{ 10, 10, 20, 10 };
	E_EXPECT_TRUE(HitTestRect(Rect, FVector2(10.2f, 10.3f), 1.0f) == ERectHandle::TopLeft);
	E_EXPECT_TRUE(HitTestRect(Rect, FVector2(29.8f, 19.9f), 1.0f) == ERectHandle::BottomRight);
	E_EXPECT_TRUE(HitTestRect(Rect, FVector2(20.0f, 10.5f), 1.0f) == ERectHandle::Top);
	E_EXPECT_TRUE(HitTestRect(Rect, FVector2(30.4f, 15.0f), 1.0f) == ERectHandle::Right);
	E_EXPECT_TRUE(HitTestRect(Rect, FVector2(20.0f, 15.0f), 1.0f) == ERectHandle::Move);
	E_EXPECT_TRUE(HitTestRect(Rect, FVector2(40.0f, 15.0f), 1.0f) == ERectHandle::None);
}

E_TEST(Sprite2DEditing_PivotAndBorder)
{
	const FPixelRect Rect{ 0, 0, 10, 20 };
	// 이미지 아래쪽 = 피벗 Y 0, 반 픽셀 스냅
	const FVector2 Pivot = PivotFromImagePoint(Rect, FVector2(5.2f, 20.0f), true);
	E_EXPECT_NEAR(Pivot.X, 0.5f, 1e-6f);
	E_EXPECT_NEAR(Pivot.Y, 0.0f, 1e-6f);
	const FVector2 Back = PivotToImagePoint(Rect, FVector2(0.25f, 0.75f));
	E_EXPECT_NEAR(Back.X, 2.5f, 1e-6f);
	E_EXPECT_NEAR(Back.Y, 5.0f, 1e-6f);
	// 테두리: 오른쪽 테두리는 오른쪽 끝에서 거리, 합이 크기를 넘지 않음
	FSpriteSlice Slice;
	Slice.W = 10;
	Slice.H = 20;
	ApplyBorderDrag(Slice, 0, FVector2(3.0f, 0.0f));
	ApplyBorderDrag(Slice, 2, FVector2(1.0f, 0.0f)); // 오른쪽에서 9 → 10 - 3 = 7로 자름
	E_EXPECT_EQ(Slice.BorderLeft, 3);
	E_EXPECT_EQ(Slice.BorderRight, 7);
	ApplyBorderDrag(Slice, 3, FVector2(0.0f, 16.0f));
	E_EXPECT_EQ(Slice.BorderBottom, 4);
	// 사각형이 줄면 테두리도 줄어든다
	SetRect(Slice, FPixelRect{ 0, 0, 5, 3 });
	E_EXPECT_EQ(Slice.BorderLeft, 3);
	E_EXPECT_EQ(Slice.BorderRight, 2);
	E_EXPECT_EQ(Slice.BorderBottom, 3);
}

E_TEST(Sprite2DEditing_UniqueNamesAndRename)
{
	FSpriteAsset Asset = MakeAtlas();
	E_EXPECT_EQ(MakeUniqueName(Asset, "D"), std::string("D"));
	E_EXPECT_EQ(MakeUniqueName(Asset, "A"), std::string("A_1"));
	Asset.Slices[1].Name = "A_1";
	E_EXPECT_EQ(MakeUniqueName(Asset, "A"), std::string("A_2"));
	E_EXPECT_EQ(MakeUniqueName(Asset, ""), std::string("Slice"));
	// 자기 자신 이름은 중복 아님
	E_EXPECT_TRUE(ValidateRename(Asset, 0, "A").empty());
	E_EXPECT_FALSE(ValidateRename(Asset, 0, "A_1").empty());
	E_EXPECT_FALSE(ValidateRename(Asset, 0, "").empty());
	std::string Error;
	E_EXPECT_FALSE(RenameSlice(Asset, 2, "A", &Error));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_TRUE(RenameSlice(Asset, 2, "Zed"));
	E_EXPECT_EQ(Asset.Slices[2].Name, std::string("Zed"));
	const int32 Added = AddSlice(Asset, FPixelRect{ 0, 0, 4, 4 }, "Zed");
	E_EXPECT_EQ(Asset.Slices[static_cast<size_t>(Added)].Name, std::string("Zed_1"));
}

E_TEST(Sprite2DEditing_NaturalSortAndPositionSort)
{
	E_EXPECT_TRUE(NaturalLess("Run_2", "Run_10"));
	E_EXPECT_FALSE(NaturalLess("Run_10", "Run_2"));
	E_EXPECT_TRUE(NaturalLess("A", "B"));
	E_EXPECT_TRUE(NaturalLess("Run", "Run_0"));
	E_EXPECT_FALSE(NaturalLess("x007", "x7"));
	FSpriteAsset Asset;
	for (const auto& [Name, X, Y] : { std::tuple{ "S10", 0, 8 }, std::tuple{ "S2", 8, 0 }, std::tuple{ "S1", 0, 0 } })
	{
		FSpriteSlice Slice;
		Slice.Name = Name;
		Slice.X    = X;
		Slice.Y    = Y;
		Asset.Slices.push_back(Slice);
	}
	SortSlices(Asset, ESliceSort::Name);
	E_EXPECT_EQ(Asset.Slices[0].Name + Asset.Slices[1].Name + Asset.Slices[2].Name, std::string("S1S2S10"));
	SortSlices(Asset, ESliceSort::Position);
	E_EXPECT_EQ(Asset.Slices[0].Name + Asset.Slices[1].Name + Asset.Slices[2].Name, std::string("S1S2S10"));
	Asset.Slices[1].Y = 20;
	SortSlices(Asset, ESliceSort::Position);
	E_EXPECT_EQ(Asset.Slices[2].Name, std::string("S2"));
}

E_TEST(Sprite2DEditing_GridSliceSkipsEmptyCells)
{
	// 4x2 셀(8px) 이미지, (1,0)과 (3,1) 칸만 투명
	const int32        Width  = 32;
	const int32        Height = 16;
	std::vector<uint8> Pixels(static_cast<size_t>(Width * Height * 4), 255);
	const auto         ClearCell = [&](int32 CellX, int32 CellY) {
		for (int32 Y = CellY * 8; Y < CellY * 8 + 8; ++Y)
		{
			for (int32 X = CellX * 8; X < CellX * 8 + 8; ++X)
			{
				Pixels[static_cast<size_t>((Y * Width + X) * 4 + 3)] = 0;
			}
		}
	};
	ClearCell(1, 0);
	ClearCell(3, 1);
	const FImageView  Image{ Width, Height, Pixels };
	FGridSliceOptions Options;
	Options.CellWidth  = 8;
	Options.CellHeight = 8;
	Options.NamePrefix = "T";
	std::vector<FSpriteSlice> Slices = SliceGridWithOptions(Width, Height, Options, Image);
	E_EXPECT_EQ(Slices.size(), static_cast<size_t>(6));
	E_EXPECT_EQ(Slices[1].X, 16); // (1,0) 건너뜀
	E_EXPECT_EQ(Slices[1].Name, std::string("T1")); // 번호는 남은 칸에 다시
	E_EXPECT_EQ(Slices.back().Name, std::string("T5"));
	// 건너뛰기 끔 / 픽셀 없음 = 전부
	Options.bSkipEmpty = false;
	E_EXPECT_EQ(SliceGridWithOptions(Width, Height, Options, Image).size(), static_cast<size_t>(8));
	Options.bSkipEmpty = true;
	E_EXPECT_EQ(SliceGridWithOptions(Width, Height, Options, FImageView{}).size(), static_cast<size_t>(8));
	// 개수 모드: 여백 1, 간격 2, 3열 → (32 - 2 - 4) / 3 = 8
	Options.bByCount = true;
	Options.Columns  = 3;
	Options.Rows     = 1;
	Options.Margin   = 1;
	Options.Spacing  = 2;
	int32 CellWidth  = 0;
	int32 CellHeight = 0;
	ComputeGridCellSize(Width, Height, Options, CellWidth, CellHeight);
	E_EXPECT_EQ(CellWidth, 8);
	E_EXPECT_EQ(CellHeight, 14);
	// 덧붙이기: 이름 충돌은 비켜 간다
	FSpriteAsset Asset;
	FSpriteSlice Existing;
	Existing.Name = "T0";
	Asset.Slices.push_back(Existing);
	Options            = FGridSliceOptions{};
	Options.CellWidth  = 16;
	Options.CellHeight = 16;
	Options.NamePrefix = "T";
	ApplyGridSlices(Asset, SliceGridWithOptions(Width, Height, Options, FImageView{}), false);
	E_EXPECT_EQ(Asset.Slices.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Asset.Slices[1].Name, std::string("T0_1"));
	ApplyGridSlices(Asset, SliceGridWithOptions(Width, Height, Options, FImageView{}), true);
	E_EXPECT_EQ(Asset.Slices.size(), static_cast<size_t>(2));
}

E_TEST(Sprite2DEditing_FrameListOperationsKeepEvents)
{
	FFlipbookAsset Asset = MakeFlipbook(5);
	Asset.Events         = { FFlipbookEvent{ 1, "E1" }, FFlipbookEvent{ 3, "E3" } };
	// 프레임 1, 3을 맨 앞으로 → 순서 1 3 0 2 4, 이벤트도 따라감
	std::vector<int32> Selection = MoveFrames(Asset, { 3, 1 }, 0);
	E_EXPECT_TRUE(SliceOrder(Asset) == (std::vector<std::string>{ "F1", "F3", "F0", "F2", "F4" }));
	E_EXPECT_TRUE(Selection == (std::vector<int32>{ 0, 1 }));
	E_EXPECT_EQ(Asset.Events[0].Frame, 0);
	E_EXPECT_EQ(Asset.Events[1].Frame, 1);
	E_EXPECT_EQ(Asset.FrameDurations.size(), static_cast<size_t>(5));
	// 끝으로 (InsertBefore = N)
	Selection = MoveFrames(Asset, { 0 }, 5);
	E_EXPECT_TRUE(SliceOrder(Asset) == (std::vector<std::string>{ "F3", "F0", "F2", "F4", "F1" }));
	E_EXPECT_TRUE(Selection == (std::vector<int32>{ 4 }));
	E_EXPECT_EQ(Asset.Events[0].Frame, 4);
	// 복제: 마지막 선택 뒤에 사본, 이벤트는 복제 안 함
	Selection = DuplicateFrames(Asset, { 0, 1 });
	E_EXPECT_TRUE(SliceOrder(Asset) == (std::vector<std::string>{ "F3", "F0", "F3", "F0", "F2", "F4", "F1" }));
	E_EXPECT_TRUE(Selection == (std::vector<int32>{ 2, 3 }));
	E_EXPECT_EQ(Asset.Events.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Asset.Events[0].Frame, 6);
	E_EXPECT_EQ(Asset.Events[1].Frame, 0);
	// 삽입: 여러 개를 한꺼번에, 뒤 이벤트는 밀림
	const std::vector<std::string> Names = { "N0", "N1" };
	Selection                            = InsertFrames(Asset, 1, Names);
	E_EXPECT_TRUE(Selection == (std::vector<int32>{ 1, 2 }));
	E_EXPECT_EQ(Asset.Frames[1].Slice, std::string("N0"));
	E_EXPECT_EQ(Asset.Events[0].Frame, 8);
	E_EXPECT_EQ(Asset.Events[1].Frame, 0);
	// 삭제: 지운 프레임의 이벤트는 사라진다
	const int32 Next = RemoveFrames(Asset, { 0, 8 });
	E_EXPECT_EQ(Asset.Frames.size(), static_cast<size_t>(7));
	E_EXPECT_TRUE(Asset.Events.empty());
	E_EXPECT_EQ(Next, 0);
	// 범위 밖/중복 선택은 정리
	E_EXPECT_TRUE(NormalizeSelection({ 5, -1, 2, 2, 99 }, 7) == (std::vector<int32>{ 2, 5 }));
	E_EXPECT_EQ(RemoveFrames(Asset, { 0, 1, 2, 3, 4, 5, 6 }), -1);
	E_EXPECT_TRUE(Asset.Frames.empty());
}

E_TEST(Sprite2DEditing_PolygonPointEditing)
{
	// 스냅 + 타일 안으로
	const FVector2 Snapped = SnapTilePoint(FVector2(3.4f, 17.8f), 16, 16, true);
	E_EXPECT_NEAR(Snapped.X, 3.0f, 1e-6f);
	E_EXPECT_NEAR(Snapped.Y, 16.0f, 1e-6f);
	const FVector2 Free = SnapTilePoint(FVector2(3.4f, -1.0f), 16, 16, false);
	E_EXPECT_NEAR(Free.X, 3.4f, 1e-6f);
	E_EXPECT_NEAR(Free.Y, 0.0f, 1e-6f);

	std::vector<FVector2> Points = { FVector2(0, 16), FVector2(16, 16), FVector2(16, 0), FVector2(0, 0) };
	E_EXPECT_TRUE(GetPolygonWarning(Points).empty());
	E_EXPECT_EQ(FindNearestPoint(Points, FVector2(15.0f, 1.0f), 2.0f), 2);
	E_EXPECT_EQ(FindNearestPoint(Points, FVector2(8.0f, 8.0f), 2.0f), -1);
	// 위 변(16,0)-(0,0) 가운데 안쪽 → 그 변 사이에 끼워짐 → 오목 경고
	const int32 Inserted = InsertPointOnNearestEdge(Points, FVector2(8.0f, 3.0f));
	E_EXPECT_EQ(Inserted, 3);
	E_EXPECT_EQ(Points.size(), static_cast<size_t>(5));
	E_EXPECT_FALSE(GetPolygonWarning(Points).empty());
	// 바깥으로 빼면 볼록
	Points[3] = FVector2(8.0f, -4.0f);
	E_EXPECT_TRUE(GetPolygonWarning(Points).empty());
	// 3점 미만, 8점 초과 경고
	E_EXPECT_FALSE(GetPolygonWarning(std::vector<FVector2>{ FVector2(0, 0), FVector2(1, 0) }).empty());
	std::vector<FVector2> Many;
	for (int32 Index = 0; Index < 9; ++Index)
	{
		const float Angle = static_cast<float>(Index) * 6.2831853f / 9.0f;
		Many.push_back(FVector2(8.0f + 6.0f * std::cos(Angle), 8.0f + 6.0f * std::sin(Angle)));
	}
	E_EXPECT_FALSE(GetPolygonWarning(Many).empty());
	// 점이 2개 이하면 끝에 덧붙임
	std::vector<FVector2> Short = { FVector2(0, 0) };
	E_EXPECT_EQ(InsertPointOnNearestEdge(Short, FVector2(4, 4)), 1);
}

E_TEST(Sprite2DEditing_TileDefinitionsNormalize)
{
	FTilesetAsset Asset;
	Asset.TextureWidth  = 64;
	Asset.TextureHeight = 32;
	FTileDefinition Tile = GetTile(Asset, 5);
	E_EXPECT_EQ(Tile.Id, 5);
	E_EXPECT_TRUE(Tile.IsDefault());
	Tile.Collision = ETileCollision::Full;
	SetTile(Asset, Tile);
	FTileDefinition Other = GetTile(Asset, 2);
	Other.Tags.push_back("Water");
	SetTile(Asset, Other);
	E_EXPECT_EQ(Asset.Tiles.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Asset.Tiles[0].Id, 2); // Id 오름차순
	// 기본값으로 되돌리면 목록에서 빠짐
	Tile.Collision = ETileCollision::None;
	SetTile(Asset, Tile);
	E_EXPECT_EQ(Asset.Tiles.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(GetTile(Asset, 2).HasTag("Water"));
}

E_TEST(Sprite2DAssetCreation_CreatesAtlasTilesetAndFlipbook)
{
	namespace fs = std::filesystem;
	fs::path Sample;
	for (fs::path Dir = fs::current_path(); !Dir.empty(); Dir = Dir.parent_path())
	{
		if (fs::exists(Dir / L"Projects/Sample/Content/Sprites/Samples/SampleAtlas.png"))
		{
			Sample = Dir / L"Projects/Sample/Content/Sprites/Samples/SampleAtlas.png";
			break;
		}
		if (Dir == Dir.parent_path())
		{
			break;
		}
	}
	E_EXPECT_FALSE(Sample.empty());
	if (Sample.empty())
	{
		return;
	}
	const fs::path Folder = fs::temp_directory_path() / L"ProjectE_Sprite2DCreationTest";
	std::error_code ErrorCode;
	fs::remove_all(Folder, ErrorCode);
	fs::create_directories(Folder);
	const fs::path Image = Folder / L"Hero.png";
	fs::copy_file(Sample, Image);
	const auto ReadText = [](const fs::path& Path) {
		std::ifstream     File(Path, std::ios::binary);
		std::stringstream Stream;
		Stream << File.rdbuf();
		return Stream.str();
	};

	E_EXPECT_TRUE(Sprite2DAssetCreation::IsImageFile(Image));
	E_EXPECT_FALSE(Sprite2DAssetCreation::IsImageFile(Folder / L"Hero.esprite"));
	std::string    Error;
	const fs::path Atlas = Sprite2DAssetCreation::CreateSpriteAtlas(Image, false, &Error);
	E_EXPECT_TRUE(Atlas.filename() == L"Hero.esprite");
	FSpriteAsset SpriteAsset;
	E_EXPECT_TRUE(FSpriteAsset::FromJsonString(ReadText(Atlas), SpriteAsset));
	E_EXPECT_EQ(SpriteAsset.Texture, std::string("Hero.png"));
	E_EXPECT_EQ(SpriteAsset.TextureWidth, 144);
	E_EXPECT_EQ(SpriteAsset.TextureHeight, 72);
	E_EXPECT_EQ(SpriteAsset.Slices.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(SpriteAsset.Slices[0].W, 144);
	// 두 번째는 겹치지 않는 이름
	E_EXPECT_TRUE(Sprite2DAssetCreation::CreateSpriteAtlas(Image, false).filename() == L"Hero1.esprite");

	const fs::path Tileset = Sprite2DAssetCreation::CreateTileset(Image, 36, 36, &Error);
	FTilesetAsset  TilesetAsset;
	E_EXPECT_TRUE(FTilesetAsset::FromJsonString(ReadText(Tileset), TilesetAsset));
	E_EXPECT_EQ(TilesetAsset.GetColumns(), 4);
	E_EXPECT_EQ(TilesetAsset.GetRows(), 2);

	const fs::path Flipbook = Folder / L"Anim.eflipbook";
	E_EXPECT_TRUE(Sprite2DAssetCreation::SaveDefaultFlipbook(Flipbook));
	FFlipbookAsset FlipbookAsset;
	E_EXPECT_TRUE(FFlipbookAsset::FromJsonString(ReadText(Flipbook), FlipbookAsset));
	E_EXPECT_TRUE(FlipbookAsset.Frames.empty());

	// 이미지가 아니면 실패 + 오류 문구
	Error.clear();
	E_EXPECT_TRUE(Sprite2DAssetCreation::CreateSpriteAtlas(Folder / L"Missing.png", false, &Error).empty());
	E_EXPECT_FALSE(Error.empty());
	fs::remove_all(Folder, ErrorCode);
}

// ---- 격자 대화 기본값 / 슬라이스 이름 변경 전파 (Phase 56 2D 에디터 후속)

E_TEST(Sprite2DEditing_EstimateGridCellSize)
{
	int32 W = 0;
	int32 H = 0;
	// 슬라이스가 있으면 가장 흔한 크기 (같은 수면 먼저 나온 것)
	FSpriteAsset Atlas = MakeAtlas(); // 8x8 셋
	Atlas.Slices[0].W  = 24;
	Atlas.Slices[0].H  = 12;
	EstimateGridCellSize(Atlas, W, H);
	E_EXPECT_TRUE(W == 8 && H == 8);
	Atlas.Slices[1].W = 24;
	Atlas.Slices[1].H = 12; // 24x12 둘, 8x8 하나
	EstimateGridCellSize(Atlas, W, H);
	E_EXPECT_TRUE(W == 24 && H == 12);

	// 슬라이스 없음: 텍스처 가로·세로를 모두 나누는 16 → 32 → 8
	FSpriteAsset Empty;
	Empty.TextureWidth  = 64;
	Empty.TextureHeight = 96;
	EstimateGridCellSize(Empty, W, H);
	E_EXPECT_TRUE(W == 16 && H == 16);
	Empty.TextureWidth  = 40;
	Empty.TextureHeight = 24;
	EstimateGridCellSize(Empty, W, H);
	E_EXPECT_TRUE(W == 8 && H == 8);
	// 공통 약수가 없으면 축마다 (없으면 그 축 전체)
	Empty.TextureWidth  = 48;
	Empty.TextureHeight = 20;
	EstimateGridCellSize(Empty, W, H);
	E_EXPECT_TRUE(W == 16 && H == 20);
	Empty.TextureWidth = Empty.TextureHeight = 0;
	EstimateGridCellSize(Empty, W, H);
	E_EXPECT_TRUE(W == 16 && H == 16);
}

E_TEST(Sprite2DEditing_DetectSliceRenames)
{
	FSpriteAsset Saved = MakeAtlas();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Saved.Slices[static_cast<size_t>(Index)].X = Index * 8;
	}
	// 같은 사각형끼리 짝 (여러 개 동시), 이름만 같은 것은 변경 아님
	FSpriteAsset Current   = Saved;
	Current.Slices[0].Name = "Alpha";
	Current.Slices[2].Name = "Gamma";
	std::vector<FSliceRename> Renames = DetectSliceRenames(Saved, Current);
	E_EXPECT_EQ(Renames.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Renames[0] == (FSliceRename{ "A", "Alpha" }));
	E_EXPECT_TRUE(Renames[1] == (FSliceRename{ "C", "Gamma" }));

	// 사각형도 바뀌었지만 사라진 이름·새 이름이 하나씩뿐이면 짝
	Current                = Saved;
	Current.Slices[1].Name = "Bee";
	Current.Slices[1].W    = 12;
	Renames                = DetectSliceRenames(Saved, Current);
	E_EXPECT_EQ(Renames.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Renames[0] == (FSliceRename{ "B", "Bee" }));

	// 지우고 새로 만든 것(사각형 다름, 둘 이상)은 변경으로 보지 않는다
	Current = Saved;
	Current.Slices.erase(Current.Slices.begin(), Current.Slices.begin() + 2);
	FSpriteSlice New1;
	New1.Name = "N1";
	New1.X    = 40;
	New1.W = New1.H = 4;
	FSpriteSlice New2 = New1;
	New2.Name         = "N2";
	New2.X            = 50;
	Current.Slices.push_back(New1);
	Current.Slices.push_back(New2);
	E_EXPECT_TRUE(DetectSliceRenames(Saved, Current).empty());
	E_EXPECT_TRUE(DetectSliceRenames(Saved, Saved).empty());

	// 동시 적용 (교환)
	const std::vector<FSliceRename> Swap = { { "A", "B" }, { "B", "A" } };
	std::string                     Name = "A";
	E_EXPECT_TRUE(ApplySliceRename(Name, Swap));
	E_EXPECT_EQ(Name, std::string("B"));
	Name = "B";
	E_EXPECT_TRUE(ApplySliceRename(Name, Swap));
	E_EXPECT_EQ(Name, std::string("A"));
	Name = "C";
	E_EXPECT_FALSE(ApplySliceRename(Name, Swap));

	E_EXPECT_TRUE(IsSameAssetPath("Sprites/Samples/A.esprite", "sprites\\samples/a.ESPRITE"));
	E_EXPECT_TRUE(IsSameAssetPath("./Sprites/A.esprite", "Sprites/A.esprite"));
	E_EXPECT_FALSE(IsSameAssetPath("Sprites/A.esprite", "Sprites/B.esprite"));
	E_EXPECT_FALSE(IsSameAssetPath("", ""));
}

E_TEST(Sprite2DEditing_SliceRenameInFlipbookAndEntityJson)
{
	const std::vector<FSliceRename> Renames = { { "Coin", "GoldCoin" } };

	// 플립북: Sprite는 이 파일 폴더 기준 — 같은 아틀라스면 프레임 이름 치환
	const std::string Flipbook = R"({ "Version": 1, "Sprite": "SampleAtlas.esprite", "Fps": 8, "Frames": [ { "Slice": "Coin" }, { "Slice": "Ruby" }, { "Slice": "Coin", "Duration": 0.25 } ] })";
	std::string       Out;
	E_EXPECT_EQ(RenameSliceRefsInFlipbook(Flipbook, "Sprites/Samples/Gems.eflipbook", "Sprites/Samples/SampleAtlas.esprite", Renames, Out), 2);
	FFlipbookAsset Parsed;
	E_EXPECT_TRUE(FFlipbookAsset::FromJsonString(Out, Parsed));
	E_EXPECT_EQ(Parsed.Frames.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Parsed.Frames[0].Slice, std::string("GoldCoin"));
	E_EXPECT_EQ(Parsed.Frames[1].Slice, std::string("Ruby"));
	E_EXPECT_EQ(Parsed.Frames[2].Slice, std::string("GoldCoin"));
	E_EXPECT_NEAR(Parsed.Frames[2].Duration, 0.25f, 1.0e-5f);
	// 다른 폴더의 플립북 = 다른 아틀라스 → 그대로
	std::string Untouched = "unchanged";
	E_EXPECT_EQ(RenameSliceRefsInFlipbook(Flipbook, "Sprites/Other/Gems.eflipbook", "Sprites/Samples/SampleAtlas.esprite", Renames, Untouched), 0);
	E_EXPECT_EQ(Untouched, std::string("unchanged"));

	// 씬/프리팹: 모든 SpriteComponent (중첩 무관), Sprite 경로가 같은 것만, 키 순서 유지
	const std::string Scene = R"({"Entities":[
		{"Name":"A","Components":{"SpriteComponent":{"Sprite":"Sprites/Samples/SampleAtlas.esprite","Slice":"Coin","Size":[1,2]},"TransformComponent":{}}},
		{"Name":"B","Components":{"SpriteComponent":{"Sprite":"Sprites/Other.esprite","Slice":"Coin"}}},
		{"Name":"C","Components":{"SpriteComponent":{"Sprite":"sprites/samples/sampleatlas.esprite","Slice":"Ruby"}}},
		{"Name":"D","Nested":[{"SpriteComponent":{"Sprite":"Sprites\\Samples\\SampleAtlas.esprite","Slice":"Coin"}}]}
	],"Version":1})";
	E_EXPECT_EQ(RenameSliceRefsInEntityJson(Scene, "Sprites/Samples/SampleAtlas.esprite", Renames, Out), 2);
	E_EXPECT_TRUE(Out.find("\"GoldCoin\"") != std::string::npos);
	E_EXPECT_TRUE(Out.find("\"Slice\": \"Coin\"") != std::string::npos); // B (다른 아틀라스)는 그대로
	E_EXPECT_TRUE(Out.find("\"Entities\"") < Out.find("\"Version\"")); // 키 순서 유지
	E_EXPECT_TRUE(Out.find("\"Sprite\"") < Out.find("\"Slice\""));
	Untouched = "unchanged";
	E_EXPECT_EQ(RenameSliceRefsInEntityJson("{ broken", "Sprites/Samples/SampleAtlas.esprite", Renames, Untouched), 0);
	E_EXPECT_EQ(RenameSliceRefsInEntityJson(Scene, "Sprites/Samples/SampleAtlas.esprite", {}, Untouched), 0);
	E_EXPECT_EQ(Untouched, std::string("unchanged"));
}

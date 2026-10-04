#include "Core/Testing/TestFramework.h"
#include "Renderer/SpriteDraw.h"
#include "Renderer/SpriteTiles.h"
#include "Renderer/TextureCompression.h"
#include "Scene/Sprite/TilemapData.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <vector>

namespace
{
	bool NearlyEqual(const FVector3& A, const FVector3& B, float Tolerance = 1.0e-4f)
	{
		return A.Equals(B, Tolerance);
	}

	// 64x32 텍스처, 16x16 타일 (4열 2행), 타일 4 = 애니메이션 (4 ↔ 5, 0.4초씩)
	FTilesetAsset MakeTileset()
	{
		FTilesetAsset Tileset;
		Tileset.Texture       = "Tiles.png";
		Tileset.TextureWidth  = 64;
		Tileset.TextureHeight = 32;
		Tileset.TileWidth     = 16;
		Tileset.TileHeight    = 16;
		Tileset.Filter        = ESpriteFilter::Point;
		FTileDefinition Water;
		Water.Id        = 4;
		Water.Animation = { { 4, 0.4f }, { 5, 0.4f } };
		Tileset.Tiles.push_back(Water);
		return Tileset;
	}

	// 사각형 매개변수 (U, V)의 로컬 위치
	FVector3 Corner(const SpriteTiles::FTileQuad& Quad, float U, float V)
	{
		return Quad.Origin + Quad.AxisX * U + Quad.AxisZ * V;
	}
} // namespace

E_TEST(SpriteTiles_QuadWithoutFlagsCoversCell)
{
	const SpriteTiles::FTileQuad Quad = SpriteTiles::ComputeTileQuad(2, -1, 0, FVector2(50.0f, 40.0f));
	E_EXPECT_TRUE(NearlyEqual(Quad.Origin, FVector3(100.0f, 0.0f, -40.0f)));
	E_EXPECT_TRUE(NearlyEqual(Quad.AxisX, FVector3(50.0f, 0.0f, 0.0f)));
	E_EXPECT_TRUE(NearlyEqual(Quad.AxisZ, FVector3(0.0f, 0.0f, 40.0f)));
}

E_TEST(SpriteTiles_FlagsMatchTransformTileUv)
{
	// 모든 플래그 조합: 텍스처 위치 p(u, v ∈ [-0.5, 0.5])가 셀 가운데 + T(p) × 크기에 그려진다 (충돌 다각형과 같은 변환)
	const FVector2 CellSize(50.0f, 30.0f);
	const FVector2 Center = TilemapMath::CellCenterToLocal(3, 1, CellSize);
	for (uint32 Mask = 0; Mask < 8; ++Mask)
	{
		const uint32 Flags = ((Mask & 1u) ? TileCell::FlipXBit : 0u) | ((Mask & 2u) ? TileCell::FlipYBit : 0u) | ((Mask & 4u) ? TileCell::Rotate90Bit : 0u);
		const SpriteTiles::FTileQuad Quad = SpriteTiles::ComputeTileQuad(3, 1, Flags, CellSize);
		for (const FVector2 Param : { FVector2(0.0f, 0.0f), FVector2(1.0f, 0.0f), FVector2(0.0f, 1.0f), FVector2(1.0f, 1.0f), FVector2(0.25f, 0.75f) })
		{
			const FVector2 Display = TilemapMath::TransformTileUv(FVector2(Param.X - 0.5f, Param.Y - 0.5f), Flags);
			const FVector3 Expected(Center.X + Display.X * CellSize.X, 0.0f, Center.Y + Display.Y * CellSize.Y);
			E_EXPECT_TRUE(NearlyEqual(Corner(Quad, Param.X, Param.Y), Expected));
		}
	}
	// 구체 값: Rotate90(반시계) — 텍스처 오른쪽 변이 위로, 위 변이 왼쪽으로
	const SpriteTiles::FTileQuad Rotated = SpriteTiles::ComputeTileQuad(0, 0, TileCell::Rotate90Bit, FVector2(10.0f, 10.0f));
	E_EXPECT_TRUE(NearlyEqual(Rotated.Origin, FVector3(10.0f, 0.0f, 0.0f)));
	E_EXPECT_TRUE(NearlyEqual(Rotated.AxisX, FVector3(0.0f, 0.0f, 10.0f)));
	E_EXPECT_TRUE(NearlyEqual(Rotated.AxisZ, FVector3(-10.0f, 0.0f, 0.0f)));
	// FlipX — 오른쪽 아래에서 왼쪽으로
	const SpriteTiles::FTileQuad Flipped = SpriteTiles::ComputeTileQuad(0, 0, TileCell::FlipXBit, FVector2(10.0f, 10.0f));
	E_EXPECT_TRUE(NearlyEqual(Flipped.Origin, FVector3(10.0f, 0.0f, 0.0f)));
	E_EXPECT_TRUE(NearlyEqual(Flipped.AxisX, FVector3(-10.0f, 0.0f, 0.0f)));
	E_EXPECT_TRUE(NearlyEqual(Flipped.AxisZ, FVector3(0.0f, 0.0f, 10.0f)));
}

E_TEST(SpriteTiles_InstanceUvAndFilter)
{
	FTilesetAsset            Tileset  = MakeTileset();
	const FSpriteInstanceGpu Instance = SpriteTiles::MakeTileInstance(0, 0, 0, 5, Tileset, FVector2(16.0f, 16.0f));
	// 타일 5 = 1열 1행 → 텍셀 경계 UV (반 텍셀 보정 없음)
	E_EXPECT_NEAR(Instance.UVRect.X, 0.25f, 1.0e-6f);
	E_EXPECT_NEAR(Instance.UVRect.Y, 0.5f, 1.0e-6f);
	E_EXPECT_NEAR(Instance.UVRect.Z, 0.5f, 1.0e-6f);
	E_EXPECT_NEAR(Instance.UVRect.W, 1.0f, 1.0e-6f);
	E_EXPECT_EQ(Instance.Flags, SpriteTiles::FlagPoint);
	E_EXPECT_EQ(Instance.TextureIndex, 0u);
	E_EXPECT_NEAR(Instance.Color.W, 1.0f, 0.0f);
	Tileset.Filter = ESpriteFilter::Linear;
	E_EXPECT_EQ(SpriteTiles::MakeTileInstance(0, 0, 0, 5, Tileset, FVector2(16.0f, 16.0f)).Flags, 0u);
	// 범위 밖 타일 = 빈 UV (안 보임)
	const FSpriteInstanceGpu Outside = SpriteTiles::MakeTileInstance(0, 0, 0, 99, Tileset, FVector2(16.0f, 16.0f));
	E_EXPECT_NEAR(Outside.UVRect.Z - Outside.UVRect.X, 0.0f, 0.0f);
}

E_TEST(SpriteTiles_AnimationFrameSelection)
{
	const FTilesetAsset    Tileset = MakeTileset();
	const FTileDefinition& Water   = *Tileset.FindTile(4);
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Water, 4, 0.0), 4);
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Water, 4, 0.39), 4);
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Water, 4, 0.41), 5); // 반열린 구간
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Water, 4, 0.81), 4); // 주기 감쌈
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Water, 4, -0.1), 5); // 음수 시간도 감쌈
	FTileDefinition Broken;
	Broken.Animation = { { 7, 0.0f }, { 2, 0.5f } }; // 길이 0 프레임은 건너뜀
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Broken, 1, 0.1), 2);
	Broken.Animation = { { 7, 0.0f } };
	E_EXPECT_EQ(SpriteTiles::SelectAnimationFrame(Broken, 1, 0.1), 1); // 모두 0 → 원래 타일
}

E_TEST(SpriteTiles_BuildChunkSplitsAnimatedCells)
{
	const FTilesetAsset                        Tileset = MakeTileset();
	const std::vector<SpriteTiles::FCellRecord> Cells   = { { 0, 0, TileCell::Make(1) },
		                                                     { 1, 0, TileCell::Make(4) }, // 애니메이션
		                                                     { 2, 0, TileCell::Make(2, TileCell::FlipXBit) },
		                                                     { 0, 1, TileCell::Make(0) } };
	SpriteTiles::FChunkBuild Build;
	SpriteTiles::BuildChunk(Cells, Tileset, FVector2(10.0f, 10.0f), Build);
	E_EXPECT_EQ(Build.Instances.size(), size_t(3));
	E_EXPECT_EQ(Build.Animated.size(), size_t(1));
	E_EXPECT_EQ(Build.Animated[0].X, 1);
	E_EXPECT_TRUE(Build.LocalBounds.IsValid());
	E_EXPECT_TRUE(NearlyEqual(Build.LocalBounds.Min, FVector3(0.0f, 0.0f, 0.0f)));
	E_EXPECT_TRUE(NearlyEqual(Build.LocalBounds.Max, FVector3(30.0f, 0.0f, 20.0f)));
	// 반전 셀은 축이 뒤집힌 사각형
	E_EXPECT_TRUE(NearlyEqual(Build.Instances[1].AxisX, FVector3(-10.0f, 0.0f, 0.0f)));
	// 해시: 같은 내용이면 같고, 값 하나만 달라도 다르다
	E_EXPECT_EQ(Build.Hash, SpriteTiles::HashCells(Cells));
	std::vector<SpriteTiles::FCellRecord> Changed = Cells;
	Changed[2].Cell = TileCell::Make(2);
	E_EXPECT_TRUE(SpriteTiles::HashCells(Changed) != Build.Hash);
}

E_TEST(SpriteDraw_BatchRunsWithChunks)
{
	// 그리기 순서: 항목 A(키 0), 항목 B(키 0), 청크 1(키 0), 항목 C(키 0), 청크 0(키 2), 항목 D(키 1), 항목 E(키 1)
	const std::vector<uint8> Keys   = { 0, 0, 0, 0, 2, 1, 1 };
	const std::vector<int32> Chunks = { -1, -1, 1, -1, 0, -1, -1 };
	std::vector<SpriteBatching::FRun> Runs;
	SpriteBatching::BuildRuns(Keys, Chunks, Runs);
	E_EXPECT_EQ(Runs.size(), size_t(5));
	E_EXPECT_TRUE(!Runs[0].bChunk && Runs[0].First == 0 && Runs[0].Count == 2);
	E_EXPECT_TRUE(Runs[1].bChunk && Runs[1].First == 1 && Runs[1].PipelineKey == 0);
	E_EXPECT_TRUE(!Runs[2].bChunk && Runs[2].First == 2 && Runs[2].Count == 1); // 청크를 사이에 둔 같은 키는 합치지 않음, First = 항목 순번
	E_EXPECT_TRUE(Runs[3].bChunk && Runs[3].First == 0 && Runs[3].PipelineKey == 2);
	E_EXPECT_TRUE(!Runs[4].bChunk && Runs[4].First == 3 && Runs[4].Count == 2 && Runs[4].PipelineKey == 1);
}

E_TEST(TextureCompression_PixelArtIsUncompressedBaseMip)
{
	FImage Image;
	Image.Width  = 8;
	Image.Height = 8;
	Image.Pixels.resize(8 * 8 * 4);
	for (size_t Index = 0; Index < Image.Pixels.size(); ++Index)
	{
		Image.Pixels[Index] = static_cast<uint8>(Index * 37);
	}
	const FCompressedTexture Texture = TextureCompression::Compress(Image, ETextureUsage::PixelArt);
	E_EXPECT_TRUE(Texture.Format == ETextureFormat::RGBA8);
	E_EXPECT_TRUE(Texture.bSRGB);
	E_EXPECT_EQ(Texture.Mips.size(), size_t(1));
	E_EXPECT_TRUE(Texture.Mips[0].Data == Image.Pixels); // 비트 그대로 (블록 오차 없음)
	// 같은 크기의 Color는 BC7 + 전체 밉
	const FCompressedTexture Color = TextureCompression::Compress(Image, ETextureUsage::Color);
	E_EXPECT_TRUE(Color.Format == ETextureFormat::BC7);
	E_EXPECT_EQ(Color.Mips.size(), size_t(4));
}

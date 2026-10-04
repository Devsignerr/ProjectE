#include "Core/Testing/TestFramework.h"
#include "Renderer/SpriteDraw.h"

#include <algorithm>
#include <random>
#include <vector>

namespace
{
	// 기준 구현: 비교 정렬 (레이어 → 순번 → 깊이 내림차순 → 제출 순서)
	std::vector<uint32> ReferenceSort(const std::vector<SpriteSorting::FKey>& Keys)
	{
		std::vector<uint32> Order(Keys.size());
		for (uint32 Index = 0; Index < Order.size(); ++Index)
		{
			Order[Index] = Index;
		}
		std::stable_sort(Order.begin(), Order.end(), [&](uint32 A, uint32 B) {
			const SpriteSorting::FKey& KA = Keys[A];
			const SpriteSorting::FKey& KB = Keys[B];
			if (KA.Layer != KB.Layer)
			{
				return KA.Layer < KB.Layer;
			}
			if (KA.Order != KB.Order)
			{
				return KA.Order < KB.Order;
			}
			return KA.Depth > KB.Depth;
		});
		return Order;
	}

	bool NearlyEqual(const FVector3& A, const FVector3& B) { return FVector3::Distance(A, B) < 1.0e-3f; }
	bool NearlyEqual(const FVector2& A, const FVector2& B) { return std::abs(A.X - B.X) < 1.0e-6f && std::abs(A.Y - B.Y) < 1.0e-6f; }
} // namespace

E_TEST(SpriteDraw_SortLayerOrderDepthSubmission)
{
	// 레이어가 먼저, 같은 레이어면 순번, 같으면 먼 것 먼저, 그것도 같으면 제출 순서
	const std::vector<SpriteSorting::FKey> Keys = {
		{ 1, 0, 100.0f }, // 0
		{ 0, 5, 100.0f }, // 1
		{ 0, 0, 100.0f }, // 2
		{ 0, 0, 300.0f }, // 3 (2보다 멀다 → 먼저)
		{ -1, 9, 1.0f },  // 4 (음수 레이어가 가장 먼저)
		{ 0, 0, 100.0f }, // 5 (2와 완전히 같음 → 2 뒤)
		{ 0, -3, 0.0f },  // 6 (음수 순번)
	};
	std::vector<uint32> Order;
	SpriteSorting::Sort(Keys, Order);
	const std::vector<uint32> Expected = { 4, 6, 3, 2, 5, 1, 0 };
	E_EXPECT_TRUE(Order == Expected);
}

E_TEST(SpriteDraw_SortMatchesReferenceAndIsStable)
{
	// 무작위 키(동률이 많게) — 기수 정렬 결과가 안정 비교 정렬과 같아야 한다. 음수 깊이·-0·같은 깊이 포함
	std::mt19937 Random(1234);
	for (int32 Round = 0; Round < 20; ++Round)
	{
		std::vector<SpriteSorting::FKey> Keys(1 + Random() % 3000);
		for (SpriteSorting::FKey& Key : Keys)
		{
			Key.Layer = static_cast<int32>(Random() % 5) - 2 + (Round % 4 == 0 ? 0x10000 * static_cast<int32>(Random() % 3) : 0);
			Key.Order = static_cast<int32>(Random() % 7) - 3;
			const uint32 DepthKind = Random() % 4;
			Key.Depth = DepthKind == 0 ? 0.0f : DepthKind == 1 ? -0.0f : static_cast<float>(static_cast<int32>(Random() % 2001) - 1000) * 0.5f;
		}
		std::vector<uint32> Order;
		SpriteSorting::Sort(Keys, Order);
		E_EXPECT_TRUE(Order == ReferenceSort(Keys));
	}
	// 모두 같은 키 = 제출 순서 그대로 (모든 단계 건너뜀)
	std::vector<SpriteSorting::FKey> Same(100, SpriteSorting::FKey{ 3, 2, 50.0f });
	std::vector<uint32>              Order;
	SpriteSorting::Sort(Same, Order);
	for (uint32 Index = 0; Index < Order.size(); ++Index)
	{
		E_EXPECT_EQ(Order[Index], Index);
	}
	// 빈 목록
	SpriteSorting::Sort({}, Order);
	E_EXPECT_TRUE(Order.empty());
}

E_TEST(SpriteDraw_DepthBitsDescending)
{
	using SpriteSorting::DepthToDescendingBits;
	E_EXPECT_TRUE(DepthToDescendingBits(10.0f) < DepthToDescendingBits(5.0f));
	E_EXPECT_TRUE(DepthToDescendingBits(5.0f) < DepthToDescendingBits(0.0f));
	E_EXPECT_TRUE(DepthToDescendingBits(0.0f) < DepthToDescendingBits(-5.0f));
	E_EXPECT_TRUE(DepthToDescendingBits(-5.0f) < DepthToDescendingBits(-10.0f));
	E_EXPECT_EQ(DepthToDescendingBits(-0.0f), DepthToDescendingBits(0.0f));
}

E_TEST(SpriteDraw_BatchRunsSplitOnPipelineChange)
{
	const std::vector<uint8> Keys = { 0, 0, 0, 3, 3, 0, 1, 1, 1, 1 };
	std::vector<SpriteBatching::FRun> Runs;
	SpriteBatching::BuildRuns(Keys, Runs);
	E_EXPECT_EQ(Runs.size(), static_cast<size_t>(4));
	if (Runs.size() == 4)
	{
		E_EXPECT_TRUE(Runs[0].First == 0 && Runs[0].Count == 3 && Runs[0].PipelineKey == 0);
		E_EXPECT_TRUE(Runs[1].First == 3 && Runs[1].Count == 2 && Runs[1].PipelineKey == 3);
		E_EXPECT_TRUE(Runs[2].First == 5 && Runs[2].Count == 1 && Runs[2].PipelineKey == 0);
		E_EXPECT_TRUE(Runs[3].First == 6 && Runs[3].Count == 4 && Runs[3].PipelineKey == 1);
	}
	SpriteBatching::BuildRuns({}, Runs);
	E_EXPECT_TRUE(Runs.empty());
	// 키 = 블렌드 × 조명 × 정지 (모두 다르고 개수 안). Masked는 정지 변형이 없다 (움직임 벡터를 쓰므로 정지 = 일반 키로 접힘)
	std::vector<uint32> All;
	for (uint32 Blend = 0; Blend < static_cast<uint32>(ESpriteBlendMode::Count); ++Blend)
	{
		const ESpriteBlendMode Mode = static_cast<ESpriteBlendMode>(Blend);
		for (const bool bLit : { false, true })
		{
			All.push_back(SpriteBatching::MakePipelineKey(Mode, bLit, false));
			if (Mode == ESpriteBlendMode::Masked)
			{
				E_EXPECT_TRUE(SpriteBatching::MakePipelineKey(Mode, bLit, true) == SpriteBatching::MakePipelineKey(Mode, bLit, false));
				continue;
			}
			All.push_back(SpriteBatching::MakePipelineKey(Mode, bLit, true));
		}
		// 렌더러가 키에서 블렌드를 되찾는 식 (Masked 구간 = 움직임 벡터 타깃)
		E_EXPECT_TRUE(SpriteBatching::MakePipelineKey(Mode, true, true) / 4u == Blend);
	}
	std::sort(All.begin(), All.end());
	E_EXPECT_TRUE(std::adjacent_find(All.begin(), All.end()) == All.end());
	E_EXPECT_TRUE(All.back() < SpriteBatching::PipelineKeyCount);
}

E_TEST(SpriteDraw_QuadCornersWithPivot)
{
	// 단위 변환: 피벗 가운데 → 로컬 X [-50, 50] × Z [-25, 25]
	FSpriteDrawItem Item;
	Item.Size  = FVector2(100.0f, 50.0f);
	Item.Pivot = FVector2(0.5f, 0.5f);
	SpriteMath::FQuad Quad = SpriteMath::ComputeQuad(Item);
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerPosition(Quad, 0.0f, 0.0f), FVector3(-50.0f, 0.0f, -25.0f)));
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerPosition(Quad, 1.0f, 1.0f), FVector3(50.0f, 0.0f, 25.0f)));
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerPosition(Quad, 1.0f, 0.0f), FVector3(50.0f, 0.0f, -25.0f)));

	// 피벗 왼쪽 아래 + 이동 + 2D 회전(화면 반시계 90° = +Y 축으로 -90°): 로컬 +X → 월드 +Z
	Item.Pivot = FVector2(0.0f, 0.0f);
	Item.World = FMatrix4x4::MakeTransform(FVector3(10.0f, 0.0f, 20.0f), FQuat::FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), -FMath::DegreesToRadians(90.0f)),
	                                       FVector3::OneVector);
	Quad = SpriteMath::ComputeQuad(Item);
	E_EXPECT_TRUE(NearlyEqual(Quad.Origin, FVector3(10.0f, 0.0f, 20.0f)));
	E_EXPECT_TRUE(NearlyEqual(Quad.AxisX, FVector3(0.0f, 0.0f, 100.0f)));
	E_EXPECT_TRUE(NearlyEqual(Quad.AxisZ, FVector3(-50.0f, 0.0f, 0.0f)));

	// 피벗 오른쪽 위 + 스케일 2
	Item.Pivot = FVector2(1.0f, 1.0f);
	Item.World = FMatrix4x4::MakeScale(2.0f);
	Quad       = SpriteMath::ComputeQuad(Item);
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerPosition(Quad, 1.0f, 1.0f), FVector3::ZeroVector));
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerPosition(Quad, 0.0f, 0.0f), FVector3(-200.0f, 0.0f, -100.0f)));
	// 앞면(AxisZ × AxisX) = +Y (2D 카메라 쪽)
	const FVector3 Front = FVector3::Cross(Quad.AxisZ, Quad.AxisX);
	E_EXPECT_TRUE(Front.Y > 0.0f && std::abs(Front.X) < 1.0e-3f && std::abs(Front.Z) < 1.0e-3f);

	// 정렬 깊이 = 가운데의 시선 거리
	Item.World = FMatrix4x4::MakeTranslation(FVector3(0.0f, -300.0f, 0.0f));
	Item.Pivot = FVector2(0.5f, 0.5f);
	E_EXPECT_NEAR(SpriteMath::ComputeSortDepth(SpriteMath::ComputeQuad(Item), FVector3(0.0f, 1000.0f, 0.0f), FVector3(0.0f, -1.0f, 0.0f)), 1300.0f, 1.0e-3f);
}

E_TEST(SpriteDraw_CornerUVAndFlip)
{
	FSpriteDrawItem Item;
	Item.UVMin = FVector2(0.25f, 0.5f);
	Item.UVMax = FVector2(0.75f, 1.0f);
	// 로컬 아래 변 = UVMax.Y, 위 변 = UVMin.Y (텍스처 v는 아래로)
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerUV(Item, 0.0f, 0.0f), FVector2(0.25f, 1.0f)));
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerUV(Item, 0.0f, 1.0f), FVector2(0.25f, 0.5f)));
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerUV(Item, 1.0f, 1.0f), FVector2(0.75f, 0.5f)));
	// 좌우 반전 = X 성분 교환
	std::swap(Item.UVMin.X, Item.UVMax.X);
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerUV(Item, 0.0f, 0.0f), FVector2(0.75f, 1.0f)));
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerUV(Item, 1.0f, 0.0f), FVector2(0.25f, 1.0f)));
	// 상하 반전 = Y 성분 교환
	std::swap(Item.UVMin.Y, Item.UVMax.Y);
	E_EXPECT_TRUE(NearlyEqual(SpriteMath::GetCornerUV(Item, 0.0f, 0.0f), FVector2(0.75f, 0.5f)));
}

// ---- 9-슬라이스 (SpriteNineSlice)

namespace
{
	bool Near2(const FVector2& A, const FVector2& B) { return std::abs(A.X - B.X) < 1.0e-4f && std::abs(A.Y - B.Y) < 1.0e-4f; }

	SpriteNineSlice::FInput MakeNineSliceInput()
	{
		// 원래 100x100cm, 테두리 왼 20 / 오른 30 / 위 10 / 아래 40, 피벗 가운데, UV 전체
		SpriteNineSlice::FInput Input;
		Input.Size         = FVector2(300.0f, 200.0f);
		Input.OriginalSize = FVector2(100.0f, 100.0f);
		Input.Pivot        = FVector2(0.5f, 0.5f);
		Input.BorderLeft   = 20.0f;
		Input.BorderRight  = 30.0f;
		Input.BorderTop    = 10.0f;
		Input.BorderBottom = 40.0f;
		Input.UVMin        = FVector2(0.0f, 0.0f);
		Input.UVMax        = FVector2(1.0f, 1.0f);
		return Input;
	}
} // namespace

E_TEST(SpriteNineSlice_ShouldSliceOnlyWithBorderAndResize)
{
	FSpriteSlice Slice;
	Slice.W = 32;
	Slice.H = 32;
	E_EXPECT_FALSE(SpriteNineSlice::ShouldSlice(Slice, FVector2(64.0f, 32.0f), FVector2(32.0f, 32.0f))); // 테두리 없음
	Slice.BorderLeft = 4;
	E_EXPECT_FALSE(SpriteNineSlice::ShouldSlice(Slice, FVector2(32.0f, 32.0f), FVector2(32.0f, 32.0f))); // 원래 크기
	E_EXPECT_TRUE(SpriteNineSlice::ShouldSlice(Slice, FVector2(64.0f, 32.0f), FVector2(32.0f, 32.0f)));
}

E_TEST(SpriteNineSlice_CornersKeepOriginalSize)
{
	std::vector<SpriteNineSlice::FPiece> Pieces;
	SpriteNineSlice::Build(MakeNineSliceInput(), Pieces);
	E_EXPECT_EQ(Pieces.size(), static_cast<size_t>(9));
	// 왼쪽 아래 모서리: 원래 크기 20 x 40, UV = 왼쪽 20% · 아래 40%
	E_EXPECT_TRUE(Near2(Pieces[0].Min, FVector2(-150.0f, -100.0f)));
	E_EXPECT_TRUE(Near2(Pieces[0].Max, FVector2(-130.0f, -60.0f)));
	E_EXPECT_TRUE(Near2(Pieces[0].UVMin, FVector2(0.0f, 0.6f)));
	E_EXPECT_TRUE(Near2(Pieces[0].UVMax, FVector2(0.2f, 1.0f)));
	// 오른쪽 위 모서리: 30 x 10
	E_EXPECT_TRUE(Near2(Pieces[8].Min, FVector2(120.0f, 90.0f)));
	E_EXPECT_TRUE(Near2(Pieces[8].Max, FVector2(150.0f, 100.0f)));
	E_EXPECT_TRUE(Near2(Pieces[8].UVMin, FVector2(0.7f, 0.0f)));
	E_EXPECT_TRUE(Near2(Pieces[8].UVMax, FVector2(1.0f, 0.1f)));
	// 가운데: 두 축으로 늘인다 (250 x 150), UV = 테두리 안쪽
	E_EXPECT_TRUE(Near2(Pieces[4].Min, FVector2(-130.0f, -60.0f)));
	E_EXPECT_TRUE(Near2(Pieces[4].Max, FVector2(120.0f, 90.0f)));
	E_EXPECT_TRUE(Near2(Pieces[4].UVMin, FVector2(0.2f, 0.1f)));
	E_EXPECT_TRUE(Near2(Pieces[4].UVMax, FVector2(0.7f, 0.6f)));
	// 왼쪽 가장자리 (가운데 줄 첫 칸): 폭 그대로 20, 세로만 늘임
	E_EXPECT_TRUE(Near2(Pieces[3].Max - Pieces[3].Min, FVector2(20.0f, 150.0f)));

	// 조각 → 항목: 로컬 사각형이 조각과 같다 (단위 월드)
	FSpriteDrawItem Base;
	const FSpriteDrawItem   Item = SpriteNineSlice::MakePieceItem(Base, Pieces[0]);
	const SpriteMath::FQuad Quad = SpriteMath::ComputeQuad(Item);
	E_EXPECT_TRUE(NearlyEqual(Quad.Origin, FVector3(-150.0f, 0.0f, -100.0f)));
	E_EXPECT_TRUE(NearlyEqual(Quad.AxisX, FVector3(20.0f, 0.0f, 0.0f)));
	E_EXPECT_TRUE(NearlyEqual(Quad.AxisZ, FVector3(0.0f, 0.0f, 40.0f)));
}

E_TEST(SpriteNineSlice_SmallerThanBordersShrinksProportionally)
{
	// 폭 25 < 왼 20 + 오른 30 → 테두리 × 0.5 (10, 15), 가운데 폭 0이라 빠짐 → 2열 × 3줄. UV는 원래 테두리 그대로
	SpriteNineSlice::FInput Input = MakeNineSliceInput();
	Input.Size.X                  = 25.0f;
	std::vector<SpriteNineSlice::FPiece> Pieces;
	SpriteNineSlice::Build(Input, Pieces);
	E_EXPECT_EQ(Pieces.size(), static_cast<size_t>(6));
	E_EXPECT_NEAR(Pieces[0].Max.X - Pieces[0].Min.X, 10.0f, 1e-4f);
	E_EXPECT_NEAR(Pieces[1].Max.X - Pieces[1].Min.X, 15.0f, 1e-4f);
	E_EXPECT_NEAR(Pieces[0].Min.X, -12.5f, 1e-4f);
	E_EXPECT_NEAR(Pieces[1].Max.X, 12.5f, 1e-4f);
	E_EXPECT_NEAR(Pieces[0].UVMax.X, 0.2f, 1e-5f);
	E_EXPECT_NEAR(Pieces[1].UVMin.X, 0.7f, 1e-5f);
}

E_TEST(SpriteNineSlice_FlipMirrorsAboutPivot)
{
	SpriteNineSlice::FInput Input = MakeNineSliceInput();
	Input.bFlipX                  = true;
	std::vector<SpriteNineSlice::FPiece> Pieces;
	SpriteNineSlice::Build(Input, Pieces);
	E_EXPECT_EQ(Pieces.size(), static_cast<size_t>(9));
	// 원래 왼쪽 아래 모서리가 오른쪽 아래로: 위치 부호 반전 + U 교환 (일반 항목 반전과 같은 사각형)
	E_EXPECT_TRUE(Near2(Pieces[0].Min, FVector2(130.0f, -100.0f)));
	E_EXPECT_TRUE(Near2(Pieces[0].Max, FVector2(150.0f, -60.0f)));
	E_EXPECT_NEAR(Pieces[0].UVMin.X, 0.2f, 1e-5f);
	E_EXPECT_NEAR(Pieces[0].UVMax.X, 0.0f, 1e-5f);
	E_EXPECT_NEAR(Pieces[0].UVMin.Y, 0.6f, 1e-5f); // 세로는 그대로
	Input.bFlipX = false;
	Input.bFlipY = true;
	SpriteNineSlice::Build(Input, Pieces);
	E_EXPECT_TRUE(Near2(Pieces[0].Min, FVector2(-150.0f, 60.0f)));
	E_EXPECT_TRUE(Near2(Pieces[0].Max, FVector2(-130.0f, 100.0f)));
	E_EXPECT_NEAR(Pieces[0].UVMin.Y, 1.0f, 1e-5f);
	E_EXPECT_NEAR(Pieces[0].UVMax.Y, 0.6f, 1e-5f);
}

E_TEST(SpriteNineSlice_TileRepeatsAndCropsLast)
{
	// 가로 테두리 20 / 20만, 원래 100 → 가운데 칸 60. 폭 200 → 가운데 160 = 60 + 60 + 40 (마지막은 UV도 2/3)
	SpriteNineSlice::FInput Input = MakeNineSliceInput();
	Input.Size                    = FVector2(200.0f, 100.0f);
	Input.BorderRight             = 20.0f;
	Input.BorderTop               = 0.0f;
	Input.BorderBottom            = 0.0f;
	Input.Mode                    = ESpriteSliceMode::Tile;
	std::vector<SpriteNineSlice::FPiece> Pieces;
	SpriteNineSlice::Build(Input, Pieces);
	E_EXPECT_EQ(Pieces.size(), static_cast<size_t>(5));
	E_EXPECT_NEAR(Pieces[1].Max.X - Pieces[1].Min.X, 60.0f, 1e-4f);
	E_EXPECT_NEAR(Pieces[1].UVMin.X, 0.2f, 1e-5f);
	E_EXPECT_NEAR(Pieces[1].UVMax.X, 0.8f, 1e-5f);
	E_EXPECT_NEAR(Pieces[3].Max.X - Pieces[3].Min.X, 40.0f, 1e-4f);
	E_EXPECT_NEAR(Pieces[3].UVMax.X, 0.6f, 1e-5f);
	E_EXPECT_NEAR(Pieces[4].Max.X, 100.0f, 1e-4f);
	// 늘이기면 가운데 한 칸
	Input.Mode = ESpriteSliceMode::Stretch;
	SpriteNineSlice::Build(Input, Pieces);
	E_EXPECT_EQ(Pieces.size(), static_cast<size_t>(3));
}

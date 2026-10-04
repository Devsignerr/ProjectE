#include "Core/Testing/TestFramework.h"
#include "Renderer/InstanceBatching.h"
#include "Renderer/LodMath.h"
#include "Renderer/ShadowCacheMath.h"

using namespace ShadowCacheMath;

// 캐시 결정: 처음/키 변경 = Direct, 한 프레임 안정 = Rebuild, 이후 같은 키 = Reuse
E_TEST(ShadowCache_DecideStabilizesBeforeCaching)
{
	FCascadeCacheState State;
	E_EXPECT_TRUE(Decide(State, 10, true) == ECacheAction::Direct);  // 처음 보는 키
	E_EXPECT_TRUE(Decide(State, 10, true) == ECacheAction::Rebuild); // 한 프레임 그대로 → 캐시 생성
	E_EXPECT_TRUE(Decide(State, 10, true) == ECacheAction::Reuse);
	E_EXPECT_TRUE(Decide(State, 10, true) == ECacheAction::Reuse);
}

// 키가 바뀌면 캐시 무효 (태양 이동·캐스케이드 스냅 이동·정적 캐스터 변경): 바뀐 프레임은 Direct, 다음에 다시 만든다
E_TEST(ShadowCache_KeyChangeInvalidates)
{
	FCascadeCacheState State;
	Decide(State, 1, true);
	Decide(State, 1, true);
	E_EXPECT_TRUE(Decide(State, 1, true) == ECacheAction::Reuse);
	E_EXPECT_TRUE(Decide(State, 2, true) == ECacheAction::Direct);
	E_EXPECT_FALSE(State.bCacheValid);
	// 옛 키로 돌아와도 캐시는 이미 무효 (낡은 내용을 다시 쓰지 않는다)
	E_EXPECT_TRUE(Decide(State, 1, true) == ECacheAction::Direct);
	E_EXPECT_TRUE(Decide(State, 1, true) == ECacheAction::Rebuild);
	E_EXPECT_TRUE(Decide(State, 1, true) == ECacheAction::Reuse);
}

// 매 프레임 키가 바뀌면(시간대 애니메이션으로 태양이 움직임) 캐시를 만들지 않는다 = 복사 비용 없음
E_TEST(ShadowCache_MovingSunNeverCaches)
{
	FCascadeCacheState State;
	for (uint64 Frame = 0; Frame < 16; ++Frame)
	{
		E_EXPECT_TRUE(Decide(State, 100 + Frame, true) == ECacheAction::Direct);
	}
	E_EXPECT_FALSE(State.bCacheValid);
}

// 끄면 Direct, 다시 켜면 처음부터 (꺼진 동안의 키를 믿지 않는다)
E_TEST(ShadowCache_DisableResets)
{
	FCascadeCacheState State;
	Decide(State, 5, true);
	Decide(State, 5, true);
	E_EXPECT_TRUE(Decide(State, 5, false) == ECacheAction::Direct);
	E_EXPECT_FALSE(State.bCacheValid);
	E_EXPECT_TRUE(Decide(State, 5, true) == ECacheAction::Direct);
	E_EXPECT_TRUE(Decide(State, 5, true) == ECacheAction::Rebuild);
}

// 정적 판정: 위치가 요구 프레임 수만큼 그대로여야 정적
E_TEST(ShadowCache_StaticFrames)
{
	E_EXPECT_FALSE(IsStatic(0, 30));
	E_EXPECT_FALSE(IsStatic(29, 30));
	E_EXPECT_TRUE(IsStatic(30, 30));
	E_EXPECT_TRUE(IsStatic(1, 1));
}

// 정적 집합 해시는 순서 무관(합), 값 하나만 달라도 다르다
E_TEST(ShadowCache_HashOrderIndependentAndSensitive)
{
	const float A[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
	const float B[4] = { 1.0f, 2.0f, 3.0f, 4.0001f };
	const uint64 HashA = Finalize(HashBytes(HashSeed, A, sizeof(A)));
	const uint64 HashB = Finalize(HashBytes(HashSeed, B, sizeof(B)));
	E_EXPECT_TRUE(HashA != HashB);
	E_EXPECT_EQ(HashA + HashB, HashB + HashA);
	// 8바이트 배수가 아닌 길이도 모든 바이트를 본다
	const uint8 C[3] = { 1, 2, 3 };
	const uint8 D[3] = { 1, 2, 4 };
	E_EXPECT_TRUE(HashBytes(HashSeed, C, 3) != HashBytes(HashSeed, D, 3));
}

// LOD 바이어스: floor(캐스케이드 × 값), 마지막 LOD에서 멈추고 고정 LOD(폴리지)/LOD 하나는 그대로
E_TEST(ShadowCache_LodBias)
{
	E_EXPECT_EQ(ComputeCascadeLodBias(0, 0.5f), 0u);
	E_EXPECT_EQ(ComputeCascadeLodBias(1, 0.5f), 0u);
	E_EXPECT_EQ(ComputeCascadeLodBias(2, 0.5f), 1u);
	E_EXPECT_EQ(ComputeCascadeLodBias(3, 0.5f), 1u);
	E_EXPECT_EQ(ComputeCascadeLodBias(3, 1.0f), 3u);
	E_EXPECT_EQ(ComputeCascadeLodBias(3, 0.0f), 0u);
	E_EXPECT_EQ(SelectShadowLod(1, 4, 1, false), 2u);
	E_EXPECT_EQ(SelectShadowLod(2, 4, 3, false), 3u); // 마지막 LOD에서 멈춤
	E_EXPECT_EQ(SelectShadowLod(1, 4, 2, true), 1u);  // bFixedLod
	E_EXPECT_EQ(SelectShadowLod(0, 1, 2, false), 0u); // LOD 없음
}

// 작은 캐스터: 경계 구 지름 < 텍셀 × 문턱이면 뺀다 (0 = 끔)
E_TEST(ShadowCache_SmallCaster)
{
	E_EXPECT_TRUE(IsCasterTooSmall(4.0f, 10.0f, 1.0f));  // 지름 8 < 텍셀 10
	E_EXPECT_FALSE(IsCasterTooSmall(5.0f, 10.0f, 1.0f)); // 지름 10 = 텍셀 10
	E_EXPECT_TRUE(IsCasterTooSmall(9.0f, 10.0f, 2.0f));  // 지름 18 < 20
	E_EXPECT_FALSE(IsCasterTooSmall(0.1f, 10.0f, 0.0f)); // 끔
}

// 메인/사전 패스 거리·화면 크기 컬링 (같은 묶음 목록이라 두 패스가 같은 집합)
E_TEST(ShadowCache_ScreenSizeCulling)
{
	E_EXPECT_TRUE(LodMath::ShouldCullInstance(0.001f, 100.0f, 0.002f, 0.0f));
	E_EXPECT_FALSE(LodMath::ShouldCullInstance(0.003f, 100.0f, 0.002f, 0.0f));
	E_EXPECT_FALSE(LodMath::ShouldCullInstance(0.001f, 100.0f, 0.0f, 0.0f)); // 끔
	E_EXPECT_TRUE(LodMath::ShouldCullInstance(1.0f, 5001.0f, 0.0f, 5000.0f));
	E_EXPECT_FALSE(LodMath::ShouldCullInstance(1.0f, 4999.0f, 0.0f, 5000.0f));
	// 같은 식: 원근 화면 크기 = 반지름 / (거리 × tan(반시야각))
	const float ScreenSize = LodMath::ComputePerspectiveScreenSize(1.0f, 1000.0f, 1.0f);
	E_EXPECT_NEAR(ScreenSize, 0.001f, 1.0e-6f);
}

// 묶음은 키(= 캐스케이드 LOD)를 기억한다 — 깊이 패스는 인스턴스 LOD 대신 키의 LOD로 그린다
E_TEST(ShadowCache_BatchKeepsLodKey)
{
	std::vector<FInstanceSortItem> Items = { { InstanceBatching::MakeKey(0, 0, 7, 2), 0.0f, 0 }, { InstanceBatching::MakeKey(0, 0, 7, 3), 0.0f, 1 } };
	std::vector<uint32>            Indices;
	std::vector<FInstanceBatch>    Batches;
	InstanceBatching::Build(Items, Indices, Batches);
	E_EXPECT_EQ(Batches.size(), size_t(2));
	E_EXPECT_EQ(InstanceBatching::GetLod(Batches[0].Key), 2u);
	E_EXPECT_EQ(InstanceBatching::GetLod(Batches[1].Key), 3u);
}

// 오래 재사용하던 캐시의 키가 바뀌면(이동 중 칸 넘김) 그 프레임에 바로 다시 그리고, 매 프레임 바뀌면 Direct로 떨어진다
E_TEST(ShadowCache_ImmediateRebuildAfterSustainedReuse)
{
	FCascadeCacheState State;
	Decide(State, 1, true);
	Decide(State, 1, true);
	Decide(State, 1, true);
	E_EXPECT_TRUE(Decide(State, 1, true) == ECacheAction::Reuse); // 재사용 2
	E_EXPECT_TRUE(Decide(State, 2, true) == ECacheAction::Rebuild);
	E_EXPECT_TRUE(Decide(State, 2, true) == ECacheAction::Reuse);
	E_EXPECT_TRUE(Decide(State, 2, true) == ECacheAction::Reuse);
	// 이제 매 프레임 바뀌는 키 (움직이는 태양): 한 번 Rebuild 뒤 Direct만
	E_EXPECT_TRUE(Decide(State, 3, true) == ECacheAction::Rebuild);
	E_EXPECT_TRUE(Decide(State, 4, true) == ECacheAction::Direct);
	E_EXPECT_TRUE(Decide(State, 5, true) == ECacheAction::Direct);
	E_EXPECT_FALSE(State.bCacheValid);
}

// 장 되살리기: 캐시 위에 아무것도 그리지 않은 장은 다음 프레임(같은 캐시 키) 되살리기 없음, 동적 2D 캐스터만 그린 장은 그 사각형만,
// 동적 메시처럼 위치를 모르는 것이 그린 장은 전체 복사
E_TEST(ShadowCache_SliceRestorePerCascade)
{
	const FTexelRect Small{ 10, 20, 30, 40 };
	FSliceCopyState  Clean;
	FSliceCopyState  Sprite;
	FSliceCopyState  Mesh;
	const auto Frame = [](FSliceCopyState& State, ECacheAction Action, uint64 Key, bool bMesh, const FTexelRect& Rect) {
		const ESliceRestore Restore = DecideSliceRestore(State, Action, Key);
		FinishSlice(State, Action, Key, bMesh, Rect);
		return Restore;
	};
	E_EXPECT_TRUE(Frame(Clean, ECacheAction::Rebuild, 7, false, {}) == ESliceRestore::Copy); // 처음 = 전체 복사
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Rebuild, 7, false, Small) == ESliceRestore::Copy);
	E_EXPECT_TRUE(Frame(Mesh, ECacheAction::Rebuild, 7, true, {}) == ESliceRestore::Copy);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		E_EXPECT_TRUE(Frame(Clean, ECacheAction::Reuse, 7, false, {}) == ESliceRestore::None);   // 이미 캐시 내용
		E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Reuse, 7, false, Small) == ESliceRestore::Rect); // 지난 사각형만
		E_EXPECT_TRUE(Frame(Mesh, ECacheAction::Reuse, 7, true, {}) == ESliceRestore::Copy);
	}
	// 되살릴 사각형 = 지난 프레임 것 (이번 것은 다음 프레임에)
	const FTexelRect Moved{ 12, 20, 32, 40 };
	E_EXPECT_TRUE(DecideSliceRestore(Sprite, ECacheAction::Reuse, 7) == ESliceRestore::Rect && Sprite.Dirty == Small);
	FinishSlice(Sprite, ECacheAction::Reuse, 7, false, Moved);
	E_EXPECT_TRUE(Sprite.Dirty == Moved);
	// 동적 캐스터가 떠난 프레임: 한 번 더 사각형, 그다음 없음
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Reuse, 7, false, {}) == ESliceRestore::Rect);
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Reuse, 7, false, {}) == ESliceRestore::None);
	// 동적 캐스터가 들어온 첫 프레임: 장이 깨끗했으므로 되살리기 없이 위에 그린다
	E_EXPECT_TRUE(Frame(Clean, ECacheAction::Reuse, 7, false, Small) == ESliceRestore::None);
	E_EXPECT_TRUE(Frame(Clean, ECacheAction::Reuse, 7, false, {}) == ESliceRestore::Rect);
	// 캐시 키가 바뀌면 전체 복사, Direct는 지우고 다 그리므로 되살리기 없음 + 다음 프레임 전체 복사
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Rebuild, 8, false, Small) == ESliceRestore::Copy);
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Direct, 8, false, Small) == ESliceRestore::None);
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Reuse, 8, false, {}) == ESliceRestore::Copy);
	E_EXPECT_TRUE(Frame(Sprite, ECacheAction::Reuse, 8, false, {}) == ESliceRestore::None);
	// 동적 메시와 2D 캐스터가 같이 그리면 전체
	E_EXPECT_TRUE(Frame(Mesh, ECacheAction::Reuse, 7, true, Small) == ESliceRestore::Copy);
	E_EXPECT_TRUE(Frame(Mesh, ECacheAction::Reuse, 7, false, {}) == ESliceRestore::Copy);
}

// 월드 경계 → 텍셀 사각형: 직교 투영 (x, y는 [-100, 100] → NDC), 바깥으로 내림/올림 + 여유, 장 안으로 자름
E_TEST(ShadowCache_TexelRectFromBounds)
{
	FMatrix4x4 ViewProjection = FMatrix4x4::Identity;
	ViewProjection.M[0][0]    = 1.0f / 100.0f;
	ViewProjection.M[1][1]    = 1.0f / 100.0f;
	ViewProjection.M[2][2]    = 1.0f / 1000.0f;
	// x [-50, 25] → u [0.25, 0.625] × 1024 = [256, 640], y [0, 50] → v = (1 - y)/2 → [0.25, 0.5] × 1024 = [256, 512]
	const FTexelRect Rect = ComputeTexelRect(FBox(FVector3(-50.0f, 0.0f, -10.0f), FVector3(25.0f, 50.0f, 10.0f)), ViewProjection, 1024, 1);
	E_EXPECT_TRUE(Rect == (FTexelRect{ 255, 255, 641, 513 }));
	// 장 밖으로 넘치면 자르고, 완전히 밖이면 빈 사각형
	const FTexelRect Clamped = ComputeTexelRect(FBox(FVector3(50.0f, -500.0f, 0.0f), FVector3(500.0f, -50.0f, 0.0f)), ViewProjection, 1024, 1);
	E_EXPECT_TRUE(Clamped == (FTexelRect{ 767, 767, 1024, 1024 }));
	E_EXPECT_TRUE(ComputeTexelRect(FBox(FVector3(200.0f, 0.0f, 0.0f), FVector3(300.0f, 10.0f, 0.0f)), ViewProjection, 1024, 1).IsEmpty());
	E_EXPECT_TRUE(ComputeTexelRect(FBox(), ViewProjection, 1024, 1).IsEmpty());
}

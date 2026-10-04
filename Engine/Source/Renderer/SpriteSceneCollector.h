#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Buffer.h"
#include "Renderer/SpriteDraw.h"
#include "Renderer/SpriteTiles.h"
#include "Scene/ResourceHandles.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class FD3D12RHI;
class FResourceManager;
class FScene;
struct FFrustum;
struct FResourceRoots;
class FTilemapData;
struct FTilesetAsset;
struct FTransformComponent;
struct FSpriteComponent;
struct FSpriteAsset;

// 씬의 2D 컴포넌트 → 스프라이트 렌더러 코어 입력 (Phase 56-4b). FSceneRenderer가 소유하고 뷰마다 RenderSceneColor에서
// 스프라이트 Prepare 직전에 Collect를 부른다 (게임 스레드 — 람다는 씬을 읽지 않는다). 에디터 뷰포트·런타임·에셋 미리보기가 모두 자동으로 그린다.
//
// 스프라이트 (FSpriteComponent + FTransformComponent, bVisible):
//   표시 = Sprite2DRuntime::ResolveSprite (같은 엔티티 FFlipbookComponent가 정한 아틀라스·슬라이스 우선). 크기 = SpriteMath::ComputeSize
//   (Size 0 = 슬라이스 px × UnitsPerPixel, 엔티티 스케일은 월드 행렬), 피벗 = 슬라이스 Pivot, UV = ComputeUvRect (TextureWidth/Height가 0이면
//   읽은 텍스처 크기). 반전 = 피벗을 지나는 축 거울 (피벗 1 - p + UV 성분 교환 — SpriteMath::ComputeQuad(Slice…)와 같은 사각형).
//   색 = sRGB → 선형(알파 그대로), 정렬 레이어 = 프로젝트 설정 SortingLayers.ResolveLayerChecked, 블렌드·컷오프 = 컴포넌트 Blend/AlphaCutoff.
//   9-슬라이스(슬라이스 Border + 원래와 다른 크기)는 조각 항목으로 나눈다 (SpriteNineSlice — 컴포넌트 SliceMode 늘이기/반복). 프러스텀 밖 사각형은 뺀다.
//   해석(라이브러리·텍스처 로드)은 순차, 항목 계산·컬링은 FParallel (엔티티 순서 = 뷰 순서 그대로 — 결정적).
// 타일맵 (FTilemapComponent + FTransformComponent): 엔티티마다 32x32 청크(FTilemapData 청크와 같은 칸)별 정적 인스턴스 버퍼 캐시.
//   청크 버퍼 = 타일맵 로컬 공간 인스턴스 (SpriteTiles.h) — 월드 행렬·색·텍스처 칸은 프레임마다 청크 머리로 넘기므로 움직여도 다시 만들지 않는다.
//   다시 만드는 조건: Runtime.Revision(디코딩·CommitTilemapData) 변경 시 청크 내용 해시가 다른 청크만, 타일셋 객체(경로·라이브러리 세대)·셀 크기가
//   바뀌면 전부. 사라진 청크·엔티티(이번 수집에 안 보인 엔티티)의 버퍼는 지연 해제(ShutdownDeferred).
//   업로드: 리소스 관리자가 비동기(Async)면 업로드 큐 + 새 버퍼가 준비될 때까지 이전 버퍼를 계속 그림(첫 생성은 준비 전에 안 그림),
//   Sync/AsyncDrain(자동 검증)이면 동기 업로드 — 만든 프레임에 바로 그린다 (화면 결정성).
//   청크 컬링 = 로컬 경계 × 월드 행렬 AABB vs 프러스텀. 애니메이션 타일 셀은 보이는 청크의 것만 프레임마다 항목으로
//   (SelectAnimationFrame, 시간 = FFrameTime 총 시간 — 에디터 편집 중에도 흐른다).
//   타일맵 블렌드·컷오프 = 컴포넌트 Blend/AlphaCutoff(청크 머리가 인스턴스 컷오프를 덮음), 필터 = 타일셋 Filter, 조명 = bLit, 레이어/순번 = 컴포넌트.
// 그림자 캐스터 (bCastShadows — Renderer/SpriteShadowRenderer.h): 같은 수집에서 캐스터 판정(ShadowCasterTest = 방향광 캐스케이드 ∪ 로컬 그림자 장
//   볼륨 — 메인 프러스텀 밖 캐스터도)을 통과한 스프라이트·타일맵 청크·애니메이션 타일을 그림자 목록(GetShadowItems/GetShadowChunks)에 따로 담는다.
//   정적 판정(방향광 그림자 캐시): 스프라이트 = 그리는 값(월드 행렬·크기·피벗·UV·텍스처·알파·컷오프·필터·9-슬라이스) 해시가 StaticFrames 수집 연속 같음
//   (엔티티 번호 칸 이력 — 메시 FMeshInstance::bShadowStatic과 같은 규칙), 타일맵 청크 = 타일맵 월드·색 알파·컷오프·Revision·타일셋·셀 크기가 연속 같음,
//   애니메이션 타일 = 항상 동적. 수집은 RenderSceneColor에서 그림자 패스 등록 전(캐스케이드·로컬 그림자 장 배정 뒤) 한 번.
// 텍스처: 에셋(.esprite/.etileset)의 Texture를 FSprite2DLibrary::ResolveReference로 Content 경로로 → 공개 LoadTexture
//   (고정 전체 밉 — 스트리밍 안 함). 용도 = 필터 Point면 ETextureUsage::PixelArt(무압축 RGBA8, 밉 0만 — 도트 번짐 없음), Linear면 Color(BC7).
//   경로 캐시(경로|용도 → 핸들)와 에셋 객체 캐시를 둔다. 수거 루트 = 최근 2번의 수집에서 쓴 텍스처 (AddRootProvider) — 안 보이게 된 씬의 텍스처는
//   다음 수거에 풀리고, 다시 쓸 때 핸들이 죽었으면(GetTexture == nullptr) 다시 LoadTexture.
class FSpriteSceneCollector
{
public:
	~FSpriteSceneCollector();

	void Init(FD3D12RHI& InRhi, FResourceManager& InResources);
	void Shutdown();

	// 게임 스레드. 결과는 다음 Collect까지 유효. ShadowCasterTest = 월드 경계가 그림자 캐스터 볼륨에 드는가 (비면 그림자 목록 없음 —
	// 여러 스레드에서 불린다: 읽기 전용), StaticFrames = 정적 캐스터가 되는 연속 수집 수 (r.Shadow.Cache.StaticFrames)
	using FCasterTest = std::function<bool(const FBox&)>;
	void Collect(FScene& Scene, const FFrustum& Frustum, const FCasterTest& ShadowCasterTest = {}, uint32 StaticFrames = 30);
	std::span<const FSpriteDrawItem>  GetItems() const { return Items; }
	std::span<const FSpriteChunkDraw> GetChunks() const { return Chunks; }
	std::span<const FSpriteDrawItem>  GetShadowItems() const { return ShadowItems; }   // bCastShadows + 캐스터 판정 통과 (bShadowStatic 채움)
	std::span<const FSpriteChunkDraw> GetShadowChunks() const { return ShadowChunks; } // 같은 조건 청크 (Bounds·bShadowStatic 채움)

	// 통계 (마지막 수집)
	uint32 GetCachedChunkCount() const;

private:
	struct FChunkGeometry
	{
		FD3D12Buffer                            Buffer;
		uint32                                  Count = 0;
		std::vector<SpriteTiles::FAnimatedCell> Animated;
		FBox                                    LocalBounds;
		uint64                                  Hash = 0;
	};
	struct FChunk
	{
		int32                           ChunkX = 0;
		int32                           ChunkY = 0;
		std::unique_ptr<FChunkGeometry> Current; // 그리는 것
		std::unique_ptr<FChunkGeometry> Pending; // 업로드 중 (비동기) — 준비되면 Current로
	};
	struct FTilemapCache
	{
		const FScene*                        Scene = nullptr;
		std::shared_ptr<const FTilesetAsset> Tileset;
		FVector2                             CellSize;
		uint32                               Revision    = 0;
		bool                                 bBuilt      = false;
		uint32                               LastCollect = 0;
		std::vector<FChunk>                  Chunks; // 청크 (Y, X) 오름차순
		// 그림자 정적 판정 (bCastShadows일 때만 갱신)
		uint64                               ShadowHash        = 0;
		uint32                               ShadowStable      = 0;
		uint32                               ShadowLastCollect = 0;
	};
	// 스프라이트 그림자 정적 판정 이력 (엔티티 번호 칸)
	struct FShadowHistory
	{
		uint64 Hash        = 0;
		uint32 Generation  = 0;
		uint32 LastCollect = 0;
		uint32 Stable      = 0;
	};
	struct FTextureEntry
	{
		FTextureHandle Handle;
		uint32         LastCollect = 0;
	};
	struct FAssetTexture
	{
		std::weak_ptr<const void> Owner;
		FTextureHandle            Handle;
		uint32                    Validated = 0; // 이 수집 번호에서 핸들 생존을 확인함
	};
	enum class ESourceState : uint8
	{
		Absent,       // 없음·숨김·표시할 슬라이스 없음
		Ready,        // 표시 확정
		NeedsResolve, // 경로·슬라이스·세대가 바뀜 — 순차 단계에서 Sprite2DRuntime::ResolveSprite
	};
	struct FSpriteSource
	{
		FSpriteComponent*                          Sprite     = nullptr;
		const FMatrix4x4*                          World      = nullptr;
		FEntity                                    Entity;
		const std::shared_ptr<const FSpriteAsset>* AssetOwner = nullptr; // 컴포넌트 런타임 안 (Asset 또는 FlipbookAtlas)
		const std::string*                         AssetPath  = nullptr; // AssetOwner의 Content 경로 (텍스처 상대 경로 기준)
		int32                                      Slice      = -1;
		FTextureHandle                             Texture;
		int32                                      TextureWidth  = 0;
		int32                                      TextureHeight = 0;
		int32                                      Layer         = 0;
		bool                                       bWarnLayer    = false; // 없는 레이어 이름 (순차 단계에서 경고 + Default)
		ESourceState                               State         = ESourceState::Absent;
	};

	static bool PeekSprite(const FSpriteComponent& Sprite, uint32 LibraryGeneration, FSpriteSource& Out);
	// 에셋(스프라이트 아틀라스/타일셋)의 텍스처 핸들 (Owner = 에셋 shared_ptr — 객체가 바뀌면 다시)
	FTextureHandle ResolveAssetTexture(const std::shared_ptr<const void>& Owner, const std::string& AssetPath, const std::string& Reference,
	                                   ESpriteFilter Filter);
	FTextureHandle LoadPathTexture(const std::string& ContentPath, ESpriteFilter Filter);
	void           CollectSprites(FScene& Scene, const FFrustum& Frustum, const FCasterTest& CasterTest, uint32 StaticFrames);
	void           CollectTilemaps(FScene& Scene, const FFrustum& Frustum, const FCasterTest& CasterTest, uint32 StaticFrames);
	void           RebuildTilemap(FTilemapCache& Cache, const FTilemapData& Data, bool bForceAll);
	std::unique_ptr<FChunkGeometry> CreateGeometry(const SpriteTiles::FChunkBuild& Build, int32 ChunkX, int32 ChunkY);
	bool           IsReady(const FChunkGeometry& Geometry) const;
	void           ReleaseChunk(FChunk& Chunk);
	void           AddResourceRoots(FResourceRoots& Roots) const;

	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;
	uint32            RootProviderId = 0;
	uint32            CollectIndex   = 0; // 수집마다 1씩 (0 = 아직 안 함)

	std::vector<FSpriteDrawItem>  Items;
	std::vector<FSpriteChunkDraw> Chunks;
	std::vector<FSpriteDrawItem>  ShadowItems;
	std::vector<FSpriteChunkDraw> ShadowChunks;

	std::vector<FSpriteSource>                     Sources;
	std::vector<FSpriteDrawItem>                   SpriteScratch;
	std::vector<uint8>                             SpriteVisible; // 비트 0 = 메인 프러스텀, 1 = 그림자 캐스터, 2 = 9-슬라이스
	std::vector<FShadowHistory>                    ShadowHistory;
	std::vector<SpriteNineSlice::FPiece>           PieceScratch;
	std::unordered_map<std::string, FTextureEntry> PathTextures;  // "Content 경로(소문자)|용도"
	std::unordered_map<const void*, FAssetTexture> AssetTextures; // 에셋 객체 → 텍스처
	std::unordered_map<uint64, FTilemapCache>      Tilemaps;      // 엔티티 ToId
	std::vector<SpriteTiles::FCellRecord>          CellScratch;
	SpriteTiles::FChunkBuild                       BuildScratch;
};

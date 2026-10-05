#include "Renderer/SpriteSceneCollector.h"

#include "Core/FrameTime.h"
#include "Core/Jobs/ParallelFor.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/LightMath.h"
#include "Renderer/ResourceCollector.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/ShadowCacheMath.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"
#include "Scene/Sprite/TilemapCollision.h"
#include "Scene/Sprite/TilemapData.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <algorithm>
#include <cctype>
#include <format>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// 수거 루트로 지키는 기간 (수집 횟수): 마지막으로 쓴 뒤 이만큼 지나지 않은 텍스처
	constexpr uint32 RootKeepCollects = 2;

	FVector4 SrgbToLinearColor(const FVector4& Color)
	{
		const auto Channel = [](float Value) { return Value == 1.0f || Value == 0.0f ? Value : LightMath::SrgbToLinear(Value); };
		return FVector4(Channel(Color.X), Channel(Color.Y), Channel(Color.Z), Color.W);
	}

	std::filesystem::path ToContentFilePath(const std::string& Path)
	{
		const std::filesystem::path Relative = FStringConv::ToWide(Path);
		if (Relative.is_absolute() || !FPaths::HasProject())
		{
			return Relative.lexically_normal();
		}
		return (FPaths::GetProjectContentDirectory() / Relative).lexically_normal();
	}

	ETextureUsage GetTextureUsage(ESpriteFilter Filter)
	{
		return Filter == ESpriteFilter::Point ? ETextureUsage::PixelArt : ETextureUsage::Color;
	}

	FBox ComputeItemBounds(const FSpriteDrawItem& Item)
	{
		const SpriteMath::FQuad Quad = SpriteMath::ComputeQuad(Item);
		FBox                    Box;
		Box.AddPoint(Quad.Origin);
		Box.AddPoint(Quad.Origin + Quad.AxisX);
		Box.AddPoint(Quad.Origin + Quad.AxisZ);
		Box.AddPoint(Quad.Origin + Quad.AxisX + Quad.AxisZ);
		return Box;
	}
} // namespace

FSpriteSceneCollector::~FSpriteSceneCollector()
{
	Shutdown();
}

void FSpriteSceneCollector::Init(FD3D12RHI& InRhi, FResourceManager& InResources)
{
	E_CHECKF(Rhi == nullptr, "스프라이트 수집기가 이미 초기화되어 있습니다");
	Rhi            = &InRhi;
	Resources      = &InResources;
	RootProviderId = Resources->AddRootProvider([this](FResourceRoots& Roots) { AddResourceRoots(Roots); });
}

void FSpriteSceneCollector::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (auto& [Id, Cache] : Tilemaps)
	{
		for (FChunk& Chunk : Cache.Chunks)
		{
			ReleaseChunk(Chunk);
		}
	}
	Tilemaps.clear();
	Resources->RemoveRootProvider(RootProviderId);
	PathTextures.clear(); // 경로 캐시 텍스처는 리소스 관리자 소유 (루트에서 빠지면 수거)
	AssetTextures.clear();
	Items.clear();
	Chunks.clear();
	ShadowItems.clear();
	ShadowChunks.clear();
	History.clear();
	Rhi       = nullptr;
	Resources = nullptr;
}

uint32 FSpriteSceneCollector::GetCachedChunkCount() const
{
	size_t Count = 0;
	for (const auto& [Id, Cache] : Tilemaps)
	{
		Count += Cache.Chunks.size();
	}
	return static_cast<uint32>(Count);
}

void FSpriteSceneCollector::AddResourceRoots(FResourceRoots& Roots) const
{
	for (const auto& [Key, Entry] : PathTextures)
	{
		if (Entry.LastCollect + RootKeepCollects > CollectIndex)
		{
			Roots.Add(Entry.Handle);
		}
	}
}

FTextureHandle FSpriteSceneCollector::LoadPathTexture(const std::string& ContentPath, ESpriteFilter Filter)
{
	const ETextureUsage Usage = GetTextureUsage(Filter);
	std::string         Key   = ContentPath;
	for (char& Char : Key)
	{
		Char = Char == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
	}
	Key += Usage == ETextureUsage::PixelArt ? "|pixel" : "|color";
	FTextureEntry& Entry = PathTextures[Key];
	if (!Entry.Handle.IsValid() || Resources->GetTexture(Entry.Handle) == nullptr)
	{
		// 처음이거나 수거됨 (경로 캐시 적중이면 대기 없음)
		Entry.Handle = Resources->LoadTexture(ToContentFilePath(ContentPath), Usage);
	}
	Entry.LastCollect = CollectIndex;
	return Entry.Handle;
}

FTextureHandle FSpriteSceneCollector::ResolveAssetTexture(const std::shared_ptr<const void>& Owner, const std::string& AssetPath, const std::string& Reference,
                                                          ESpriteFilter Filter)
{
	FAssetTexture& Entry     = AssetTextures[Owner.get()];
	const bool     bSameOwner = !Entry.Owner.owner_before(Owner) && !Owner.owner_before(Entry.Owner) && !Entry.Owner.expired();
	if (bSameOwner && Entry.Validated == CollectIndex)
	{
		return Entry.Handle;
	}
	if (!bSameOwner)
	{
		Entry.Owner  = Owner;
		Entry.Handle = FTextureHandle();
	}
	const std::string ContentPath = FSprite2DLibrary::ResolveReference(AssetPath, Reference);
	Entry.Handle    = ContentPath.empty() ? FTextureHandle() : LoadPathTexture(ContentPath, Filter);
	Entry.Validated = CollectIndex;
	return Entry.Handle;
}

void FSpriteSceneCollector::Collect(FScene& Scene, const FFrustum& Frustum, const FCasterTest& ShadowCasterTest, uint32 StaticFrames,
                                    const FBillboardView& Billboard)
{
	Items.clear();
	Chunks.clear();
	ShadowItems.clear();
	ShadowChunks.clear();
	if (Rhi == nullptr)
	{
		return;
	}
	++CollectIndex;
	// 죽은 에셋 객체 항목 정리 (가끔)
	if ((CollectIndex & 255u) == 0)
	{
		std::erase_if(AssetTextures, [](const auto& Pair) { return Pair.second.Owner.expired(); });
	}
	// 타일맵 먼저: 애니메이션 타일 항목이 스프라이트 항목보다 제출 순서가 앞 (같은 키면 타일 아래)
	CollectTilemaps(Scene, Frustum, ShadowCasterTest, StaticFrames);
	CollectSprites(Scene, Frustum, ShadowCasterTest, StaticFrames, Billboard);
}

bool FSpriteSceneCollector::PeekSprite(const FSpriteComponent& Sprite, uint32 LibraryGeneration, FSpriteSource& Out)
{
	// 이미 해석된 표시를 런타임에서 바로 읽는다 (Sprite2DRuntime::ResolveSprite와 같은 판정 — 읽기만, 문자열·shared_ptr 복사 없음).
	// 플립북이 정했으면 그것, 아니면 경로·슬라이스·라이브러리 세대가 그대로일 때만. false = ResolveSprite로 다시 해석해야 함
	const FSpriteRuntime& Runtime = Sprite.Runtime;
	if (Runtime.FlipbookAtlas != nullptr)
	{
		Out.AssetOwner = &Runtime.FlipbookAtlas;
		Out.AssetPath  = &Runtime.FlipbookAtlasPath;
		Out.Slice      = Runtime.FlipbookSliceIndex;
		return true;
	}
	if (Runtime.LoadedPath != Sprite.Sprite || Runtime.Generation != LibraryGeneration || (Runtime.Asset != nullptr && Runtime.ResolvedSlice != Sprite.Slice))
	{
		return false;
	}
	Out.AssetOwner = &Runtime.Asset;
	Out.AssetPath  = &Sprite.Sprite;
	Out.Slice      = Runtime.SliceIndex;
	return true;
}

void FSpriteSceneCollector::CollectSprites(FScene& Scene, const FFrustum& Frustum, const FCasterTest& CasterTest, uint32 StaticFrames,
                                           const FBillboardView& Billboard)
{
	FRegistry&                       Registry     = Scene.GetRegistry();
	const std::vector<FEntity>*      ViewEntities = Registry.View<FSpriteComponent, FTransformComponent>().GetIterationEntities();
	if (ViewEntities == nullptr || ViewEntities->empty())
	{
		return;
	}
	TSparseSet<FSpriteComponent>*    SpritePool    = Registry.TryGetPool<FSpriteComponent>();
	TSparseSet<FTransformComponent>* TransformPool = Registry.TryGetPool<FTransformComponent>();
	const uint32                     ViewCount     = static_cast<uint32>(ViewEntities->size());
	const FSortingLayerSettings&     Layers        = FProjectSettings::Get().SortingLayers;
	const uint32                     Generation    = FSprite2DLibrary::Get().GetGeneration();

	// 1) 병렬 (읽기만): 뷰 칸마다 컴포넌트 + 이미 해석된 표시 + 정렬 레이어 (View.Each와 같은 순서·조건)
	Sources.resize(ViewCount);
	FParallel::ParallelFor(ViewCount, 512, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			FSpriteSource& Source = Sources[Index];
			Source                = FSpriteSource{};
			const FEntity          Entity    = (*ViewEntities)[Index];
			FSpriteComponent*      Sprite    = SpritePool->TryGet(Entity);
			FTransformComponent*   Transform = TransformPool->TryGet(Entity);
			if (Sprite == nullptr || Transform == nullptr || !Sprite->bVisible)
			{
				continue;
			}
			Source.Sprite = Sprite;
			Source.World  = &Transform->WorldMatrix;
			Source.Entity = Entity;
			Source.State  = PeekSprite(*Sprite, Generation, Source) ? ESourceState::Ready : ESourceState::NeedsResolve;
			// 레이어: 읽기 전용 찾기 (없는 이름 경고는 아래 순차 단계 — ResolveLayerChecked)
			const int32 Layer = Sprite->SortingLayer.empty() ? 0 : Layers.FindLayer(Sprite->SortingLayer);
			Source.Layer      = Layer;
			Source.bWarnLayer = Layer < 0;
		}
	});

	// 2) 순차: 다시 해석할 것(라이브러리), 없는 레이어 경고, 에셋별 텍스처(경로 캐시·LoadTexture) — 보통 에셋 몇 개라 직전 에셋 비교로 끝난다
	const void*    LastAsset = nullptr;
	FTextureHandle LastTexture;
	int32          LastWidth  = 0;
	int32          LastHeight = 0;
	for (FSpriteSource& Source : Sources)
	{
		if (Source.State == ESourceState::Absent)
		{
			continue;
		}
		if (Source.State == ESourceState::NeedsResolve)
		{
			Sprite2DRuntime::ResolveSprite(*Source.Sprite);
			PeekSprite(*Source.Sprite, Generation, Source); // 해석 직후라 항상 성공
		}
		if (Source.bWarnLayer)
		{
			Source.Layer = static_cast<int32>(Layers.ResolveLayerChecked(Source.Sprite->SortingLayer));
		}
		const FSpriteAsset* Asset = Source.AssetOwner->get();
		if (Asset == nullptr || Source.Slice < 0 || Source.Slice >= static_cast<int32>(Asset->Slices.size()))
		{
			Source.State = ESourceState::Absent;
			continue;
		}
		Source.State = ESourceState::Ready;
		if (Asset != LastAsset)
		{
			LastAsset   = Asset;
			LastTexture = ResolveAssetTexture(*Source.AssetOwner, *Source.AssetPath, Asset->Texture, Asset->Filter);
			LastWidth   = Asset->TextureWidth;
			LastHeight  = Asset->TextureHeight;
			if (LastWidth <= 0 || LastHeight <= 0)
			{
				// 크기를 적지 않은 에셋: 읽은 텍스처 크기 (준비 전 1x1 대체 텍스처면 다음 프레임에 맞춰진다)
				const FD3D12Texture& Texture = Resources->ResolveTexture(LastTexture);
				LastWidth                    = static_cast<int32>(Texture.GetWidth());
				LastHeight                   = static_cast<int32>(Texture.GetHeight());
			}
		}
		Source.Texture       = LastTexture;
		Source.TextureWidth  = LastWidth;
		Source.TextureHeight = LastHeight;
	}

	// 이력(그림자 정적 판정 + TAA): 엔티티 번호 칸을 병렬 전에 맞춘다 (병렬 본문은 자기 엔티티 칸만 쓴다)
	const bool bShadows = static_cast<bool>(CasterTest);
	{
		uint32 MaxIndex = 0;
		bool   bAny     = false;
		for (const FSpriteSource& Source : Sources)
		{
			if (Source.State == ESourceState::Ready)
			{
				MaxIndex = std::max(MaxIndex, Source.Entity.Index);
				bAny     = true;
			}
		}
		if (bAny && MaxIndex >= History.size())
		{
			History.resize(static_cast<size_t>(MaxIndex) + 1);
		}
	}

	// 3) 병렬: 항목 + 컬링 (자기 칸만). 에셋은 컴포넌트 런타임이 shared_ptr로 붙잡고 있어 이 수집 동안 살아 있다
	SpriteScratch.resize(ViewCount);
	SpriteVisible.resize(ViewCount);
	FParallel::ParallelFor(ViewCount, 512, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			const FSpriteSource& Source = Sources[Index];
			SpriteVisible[Index]        = 0;
			if (Source.State != ESourceState::Ready)
			{
				continue;
			}
			const FSpriteComponent& Sprite = *Source.Sprite;
			const FSpriteAsset&     Asset  = **Source.AssetOwner;
			const FSpriteSlice&     Slice  = Asset.Slices[static_cast<size_t>(Source.Slice)];
			const FSpriteUvRect     Uv     = SpriteMath::ComputeUvRect(Slice, Source.TextureWidth, Source.TextureHeight);
			FSpriteDrawItem&        Item   = SpriteScratch[Index];
			Item.World = *Source.World;
			if (Sprite.Billboard != 0 && Billboard.bValid)
			{
				// 빌보드: 위치·스케일은 엔티티, 축 방향은 카메라 (이력 해시·직전 월드도 이 행렬 — 카메라가 돌면 그리는 값이 바뀐 것)
				Item.World = SpriteMath::ComputeBillboardWorld(*Source.World, static_cast<ESpriteBillboard>(Sprite.Billboard), Billboard.Right, Billboard.Forward,
				                                               Billboard.Up);
			}
			Item.Size  = SpriteMath::ComputeSize(Slice, Asset.UnitsPerPixel, Sprite.Size);
			// 9-슬라이스 판정 (조각은 아래 4) 순차 단계에서 — 보통 소수)
			const FVector2 OriginalSize(static_cast<float>(Slice.W) * Asset.UnitsPerPixel, static_cast<float>(Slice.H) * Asset.UnitsPerPixel);
			const bool     bSliced = SpriteNineSlice::ShouldSlice(Slice, Item.Size, OriginalSize);
			Item.Pivot = Slice.Pivot;
			Item.UVMin = FVector2(Uv.U0, Uv.V0);
			Item.UVMax = FVector2(Uv.U1, Uv.V1);
			if (Sprite.bFlipX)
			{
				Item.Pivot.X = 1.0f - Item.Pivot.X;
				std::swap(Item.UVMin.X, Item.UVMax.X);
			}
			if (Sprite.bFlipY)
			{
				Item.Pivot.Y = 1.0f - Item.Pivot.Y;
				std::swap(Item.UVMin.Y, Item.UVMax.Y);
			}
			Item.Texture      = Source.Texture;
			Item.Color        = SrgbToLinearColor(Sprite.Color);
			Item.SortLayer    = Source.Layer;
			Item.OrderInLayer = Sprite.OrderInLayer;
			Item.Blend        = Sprite.Blend >= ESpriteBlendMode::Alpha && Sprite.Blend < ESpriteBlendMode::Count ? Sprite.Blend : ESpriteBlendMode::Alpha;
			Item.Filter       = Asset.Filter;
			Item.AlphaCutoff  = Sprite.AlphaCutoff;
			Item.bLit         = Sprite.bLit;
			Item.bCastShadows = Sprite.bCastShadows;
			// 조각들은 원래 사각형을 정확히 덮으므로 컬링은 원래 사각형으로
			const FBox Bounds = ComputeItemBounds(Item);
			uint8      Flags  = Frustum.Intersects(Bounds) ? 1 : 0;
			{
				// 이력 (보이든 아니든 매 수집 갱신 — 끊기지 않게). 그림자 해시 = 그림자 결과를 바꾸는 값, 그리는 해시 = 거기에 화면 색·정렬·파이프라인
				using namespace ShadowCacheMath;
				uint64 Hash = HashSeed;
				Hash        = HashValue(Hash, Item.World);
				Hash        = HashValue(Hash, Item.Size);
				Hash        = HashValue(Hash, Item.Pivot);
				Hash        = HashValue(Hash, Item.UVMin);
				Hash        = HashValue(Hash, Item.UVMax);
				Hash        = HashValue(Hash, Item.Texture);
				Hash        = HashValue(Hash, Item.Color.W);
				Hash        = HashValue(Hash, Item.AlphaCutoff);
				Hash        = HashValue(Hash, Item.Filter);
				Hash        = HashValue(Hash, bSliced ? static_cast<int32>(Sprite.SliceMode) + 1 : 0);
				uint64 DrawHash = HashValue(Hash, Item.Color);
				DrawHash        = HashValue(DrawHash, Item.SortLayer);
				DrawHash        = HashValue(DrawHash, Item.OrderInLayer);
				DrawHash        = HashValue(DrawHash, Item.Blend);
				DrawHash        = HashValue(DrawHash, static_cast<uint32>(Item.bLit));
				FSpriteHistory& Entry       = History[Source.Entity.Index];
				const bool      bContinuous = Entry.Generation == Source.Entity.Generation && Entry.LastCollect + 1 == CollectIndex;
				Entry.Stable                = bContinuous && Entry.ShadowHash == Hash ? std::min(Entry.Stable + 1, 0x7FFFFFFFu) : 0u;
				Item.PrevWorld              = bContinuous ? Entry.LastWorld : Item.World;
				Item.bHasPrevWorld          = true;
				Item.bStatic                = bContinuous && Entry.DrawHash == DrawHash;
				Entry.ShadowHash            = Hash;
				Entry.DrawHash              = DrawHash;
				Entry.LastWorld             = Item.World;
				Entry.Generation            = Source.Entity.Generation;
				Entry.LastCollect           = CollectIndex;
				if (bShadows && Sprite.bCastShadows)
				{
					// 그림자 정적 판정: 그림자 해시가 연속 수집에서 같았던 횟수
					Item.bShadowStatic = IsStatic(Entry.Stable, StaticFrames);
					Flags |= CasterTest(Bounds) ? 2 : 0;
				}
			}
			if (bSliced && (Flags & 3) != 0)
			{
				Flags |= 4;
			}
			SpriteVisible[Index] = Flags;
		}
	});

	// 4) 보이는 것만 뷰 순서대로 (9-슬라이스는 조각 항목들 — 같은 정렬 키·연속 제출이라 한 구간으로 묶인다)
	Items.reserve(Items.size() + ViewCount);
	for (uint32 Index = 0; Index < ViewCount; ++Index)
	{
		const uint8 Flags = SpriteVisible[Index];
		if (Flags == 0)
		{
			continue;
		}
		const FSpriteDrawItem& Item = SpriteScratch[Index];
		if ((Flags & 4) == 0)
		{
			if ((Flags & 1) != 0)
			{
				Items.push_back(Item);
			}
			if ((Flags & 2) != 0)
			{
				ShadowItems.push_back(Item);
			}
			continue;
		}
		const FSpriteSource&    Source = Sources[Index];
		const FSpriteComponent& Sprite = *Source.Sprite;
		const FSpriteAsset&     Asset  = **Source.AssetOwner;
		const FSpriteSlice&     Slice  = Asset.Slices[static_cast<size_t>(Source.Slice)];
		const FSpriteUvRect     Uv     = SpriteMath::ComputeUvRect(Slice, Source.TextureWidth, Source.TextureHeight);
		SpriteNineSlice::FInput Input;
		Input.Size         = Item.Size;
		Input.OriginalSize = FVector2(static_cast<float>(Slice.W) * Asset.UnitsPerPixel, static_cast<float>(Slice.H) * Asset.UnitsPerPixel);
		Input.Pivot        = Slice.Pivot;
		Input.BorderLeft   = static_cast<float>(Slice.BorderLeft) * Asset.UnitsPerPixel;
		Input.BorderTop    = static_cast<float>(Slice.BorderTop) * Asset.UnitsPerPixel;
		Input.BorderRight  = static_cast<float>(Slice.BorderRight) * Asset.UnitsPerPixel;
		Input.BorderBottom = static_cast<float>(Slice.BorderBottom) * Asset.UnitsPerPixel;
		Input.UVMin        = FVector2(Uv.U0, Uv.V0);
		Input.UVMax        = FVector2(Uv.U1, Uv.V1);
		Input.bFlipX       = Sprite.bFlipX;
		Input.bFlipY       = Sprite.bFlipY;
		Input.Mode         = Sprite.SliceMode;
		SpriteNineSlice::Build(Input, PieceScratch);
		for (const SpriteNineSlice::FPiece& Piece : PieceScratch)
		{
			const FSpriteDrawItem PieceItem = SpriteNineSlice::MakePieceItem(Item, Piece);
			if ((Flags & 1) != 0)
			{
				Items.push_back(PieceItem);
			}
			if ((Flags & 2) != 0)
			{
				ShadowItems.push_back(PieceItem);
			}
		}
	}
}

// ---------------------------------------------------------------- 타일맵

bool FSpriteSceneCollector::IsReady(const FChunkGeometry& Geometry) const
{
	return Geometry.Count == 0 || Geometry.Buffer.GetUploadFence() <= Rhi->GetUploadQueue().GetFinalizedFence();
}

void FSpriteSceneCollector::ReleaseChunk(FChunk& Chunk)
{
	for (std::unique_ptr<FChunkGeometry>* Geometry : { &Chunk.Current, &Chunk.Pending })
	{
		if (*Geometry != nullptr && (*Geometry)->Buffer.GetResource() != nullptr)
		{
			(*Geometry)->Buffer.ShutdownDeferred(*Rhi);
		}
		Geometry->reset();
	}
}

std::unique_ptr<FSpriteSceneCollector::FChunkGeometry> FSpriteSceneCollector::CreateGeometry(const SpriteTiles::FChunkBuild& Build, int32 ChunkX,
                                                                                              int32 ChunkY)
{
	auto Geometry         = std::make_unique<FChunkGeometry>();
	Geometry->Animated    = Build.Animated;
	Geometry->LocalBounds = Build.LocalBounds;
	Geometry->Hash        = Build.Hash;
	if (Build.Instances.empty())
	{
		return Geometry;
	}
	const std::wstring Name = std::format(L"TilemapChunk({},{})", ChunkX, ChunkY);
	const uint64       Size = sizeof(FSpriteInstanceGpu) * Build.Instances.size();
	// 비동기 로딩이면 업로드 큐 (준비 전에는 이전 버퍼를 그림), 동기·자동 검증(비우기)이면 바로 — 만든 프레임에 그린다
	const bool bCreated = Resources->GetLoadMode() == EResourceLoadMode::Async
	                          ? Geometry->Buffer.InitStaticAsync(Rhi->GetDevice(), Rhi->GetUploadQueue(), Build.Instances.data(), Size, Name.c_str())
	                          : Geometry->Buffer.InitStatic(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Build.Instances.data(), Size, Name.c_str());
	if (!bCreated)
	{
		E_LOG(LogRenderer, Error, "타일맵 청크 버퍼를 만들지 못했습니다 ({}, {})", ChunkX, ChunkY);
		Geometry->Animated.clear();
		Geometry->LocalBounds = FBox();
		return Geometry;
	}
	Geometry->Count = static_cast<uint32>(Build.Instances.size());
	return Geometry;
}

void FSpriteSceneCollector::RebuildTilemap(FTilemapCache& Cache, const FTilemapData& Data, bool bForceAll)
{
	// 셀을 청크별로 묶어 (ForEachCell = 청크 (Y, X) 오름차순 → 청크 안 행 우선) 내용 해시가 바뀐 청크만 다시 만든다
	std::vector<FChunk> Next;
	Next.reserve(Data.GetChunkCount());
	size_t     OldIndex = 0;
	const auto Flush    = [&](int32 ChunkX, int32 ChunkY) {
		if (CellScratch.empty())
		{
			return;
		}
		// 기존 청크 찾기 (둘 다 (Y, X) 오름차순 — 지나간 것은 사라진 청크)
		while (OldIndex < Cache.Chunks.size() &&
		       std::pair(Cache.Chunks[OldIndex].ChunkY, Cache.Chunks[OldIndex].ChunkX) < std::pair(ChunkY, ChunkX))
		{
			ReleaseChunk(Cache.Chunks[OldIndex++]);
		}
		FChunk Chunk;
		Chunk.ChunkX = ChunkX;
		Chunk.ChunkY = ChunkY;
		if (OldIndex < Cache.Chunks.size() && Cache.Chunks[OldIndex].ChunkX == ChunkX && Cache.Chunks[OldIndex].ChunkY == ChunkY)
		{
			Chunk = std::move(Cache.Chunks[OldIndex++]);
		}
		const FChunkGeometry* Latest = Chunk.Pending != nullptr ? Chunk.Pending.get() : Chunk.Current.get();
		if (bForceAll || Latest == nullptr || Latest->Hash != SpriteTiles::HashCells(CellScratch))
		{
			SpriteTiles::BuildChunk(CellScratch, *Cache.Tileset, Cache.CellSize, BuildScratch);
			std::unique_ptr<FChunkGeometry> Geometry = CreateGeometry(BuildScratch, ChunkX, ChunkY);
			if (Chunk.Pending != nullptr && Chunk.Pending->Buffer.GetResource() != nullptr)
			{
				Chunk.Pending->Buffer.ShutdownDeferred(*Rhi);
			}
			Chunk.Pending.reset();
			if (Chunk.Current == nullptr || IsReady(*Geometry))
			{
				if (Chunk.Current != nullptr && Chunk.Current->Buffer.GetResource() != nullptr)
				{
					Chunk.Current->Buffer.ShutdownDeferred(*Rhi);
				}
				Chunk.Current = std::move(Geometry);
			}
			else
			{
				Chunk.Pending = std::move(Geometry); // 업로드가 끝날 때까지 이전 내용을 그린다
			}
		}
		Next.push_back(std::move(Chunk));
		CellScratch.clear();
	};

	CellScratch.clear();
	int32 CurrentX = 0;
	int32 CurrentY = 0;
	Data.ForEachCell([&](int32 X, int32 Y, uint32 Cell) {
		const int32 ChunkX = X >> FTilemapData::ChunkShift;
		const int32 ChunkY = Y >> FTilemapData::ChunkShift;
		if (!CellScratch.empty() && (ChunkX != CurrentX || ChunkY != CurrentY))
		{
			Flush(CurrentX, CurrentY);
		}
		CurrentX = ChunkX;
		CurrentY = ChunkY;
		CellScratch.push_back({ X, Y, Cell });
	});
	Flush(CurrentX, CurrentY);
	for (; OldIndex < Cache.Chunks.size(); ++OldIndex)
	{
		ReleaseChunk(Cache.Chunks[OldIndex]);
	}
	Cache.Chunks = std::move(Next);
}

void FSpriteSceneCollector::CollectTilemaps(FScene& Scene, const FFrustum& Frustum, const FCasterTest& CasterTest, uint32 StaticFrames)
{
	const FSortingLayerSettings& Layers = FProjectSettings::Get().SortingLayers;
	const double                 Time   = FFrameTime::GetTotalSeconds();
	Scene.GetRegistry().View<FTilemapComponent, FTransformComponent>().Each([&](FEntity Entity, FTilemapComponent& Tilemap, FTransformComponent& Transform) {
		const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap);
		if (Tileset == nullptr)
		{
			return; // 캐시 항목은 이번 수집에 안 보였으므로 아래에서 정리된다
		}
		const FTilemapData& Data     = Sprite2DRuntime::GetTilemapData(Tilemap);
		const FVector2      CellSize = TilemapCollision::ResolveCellSize(*Tileset, Tilemap.CellSize);
		FTilemapCache&      Cache    = Tilemaps[Entity.ToId()];
		if (Cache.Scene != &Scene)
		{
			for (FChunk& Chunk : Cache.Chunks)
			{
				ReleaseChunk(Chunk);
			}
			Cache        = FTilemapCache{};
			Cache.Scene  = &Scene;
		}
		Cache.LastCollect      = CollectIndex;
		const bool bParameters = !Cache.bBuilt || Cache.Tileset != Tileset || Cache.CellSize.X != CellSize.X || Cache.CellSize.Y != CellSize.Y;
		if (bParameters || Cache.Revision != Tilemap.Runtime.Revision)
		{
			Cache.Tileset  = Tileset;
			Cache.CellSize = CellSize;
			Cache.Revision = Tilemap.Runtime.Revision;
			Cache.bBuilt   = true;
			RebuildTilemap(Cache, Data, bParameters);
		}

		const FTextureHandle   Texture = ResolveAssetTexture(Tileset, Tilemap.Tileset, Tileset->Texture, Tileset->Filter);
		const FVector4         Color   = SrgbToLinearColor(Tilemap.Color);
		const int32            Layer   = static_cast<int32>(Layers.ResolveLayerChecked(Tilemap.SortingLayer));
		const FMatrix4x4&      World   = Transform.WorldMatrix;
		const ESpriteBlendMode Blend   = Tilemap.Blend >= ESpriteBlendMode::Alpha && Tilemap.Blend < ESpriteBlendMode::Count ? Tilemap.Blend : ESpriteBlendMode::Alpha;
		const bool             bShadow = CasterTest && Tilemap.bCastShadows;
		bool                   bShadowStatic = false;
		// TAA 이력 (매 수집): 그리는 값이 직전 수집과 같은가 + 직전 월드
		bool       bDrawStatic = false;
		FMatrix4x4 PrevWorld   = World;
		{
			using namespace ShadowCacheMath;
			uint64 Hash = HashSeed;
			Hash        = HashValue(Hash, World);
			Hash        = HashValue(Hash, Color);
			Hash        = HashValue(Hash, Tilemap.AlphaCutoff);
			Hash        = HashValue(Hash, Cache.Revision);
			Hash        = HashValue(Hash, Tileset.get());
			Hash        = HashValue(Hash, CellSize);
			Hash        = HashValue(Hash, Texture);
			Hash        = HashValue(Hash, Layer);
			Hash        = HashValue(Hash, Tilemap.OrderInLayer);
			Hash        = HashValue(Hash, Blend);
			Hash        = HashValue(Hash, static_cast<uint32>(Tilemap.bLit));
			const bool bContinuous = Cache.DrawLastCollect + 1 == CollectIndex;
			bDrawStatic            = bContinuous && Cache.DrawHash == Hash;
			PrevWorld              = bContinuous ? Cache.LastWorld : World;
			Cache.DrawHash         = Hash;
			Cache.LastWorld        = World;
			Cache.DrawLastCollect  = CollectIndex;
		}
		if (bShadow)
		{
			// 정적 판정: 그림자 결과를 바꾸는 타일맵 값이 연속 수집에서 같았던 횟수 (청크 내용 변경은 Revision이 잡는다)
			using namespace ShadowCacheMath;
			uint64 Hash = HashSeed;
			Hash        = HashValue(Hash, World);
			Hash        = HashValue(Hash, Color.W);
			Hash        = HashValue(Hash, Tilemap.AlphaCutoff);
			Hash        = HashValue(Hash, Cache.Revision);
			Hash        = HashValue(Hash, Tileset.get());
			Hash        = HashValue(Hash, CellSize);
			Hash        = HashValue(Hash, Texture);
			const bool bContinuous  = Cache.ShadowLastCollect + 1 == CollectIndex;
			Cache.ShadowStable      = bContinuous && Cache.ShadowHash == Hash ? std::min(Cache.ShadowStable + 1, 0x7FFFFFFFu) : 0u;
			Cache.ShadowHash        = Hash;
			Cache.ShadowLastCollect = CollectIndex;
			bShadowStatic           = IsStatic(Cache.ShadowStable, StaticFrames);
		}
		for (FChunk& Chunk : Cache.Chunks)
		{
			// 업로드가 끝난 새 내용으로 바꾼다
			if (Chunk.Pending != nullptr && IsReady(*Chunk.Pending))
			{
				if (Chunk.Current != nullptr && Chunk.Current->Buffer.GetResource() != nullptr)
				{
					Chunk.Current->Buffer.ShutdownDeferred(*Rhi);
				}
				Chunk.Current = std::move(Chunk.Pending);
			}
			if (Chunk.Current == nullptr || !IsReady(*Chunk.Current) || !Chunk.Current->LocalBounds.IsValid())
			{
				continue;
			}
			const FBox Bounds = Chunk.Current->LocalBounds.TransformBy(World);
			const bool bMain  = Frustum.Intersects(Bounds);
			const bool bCast  = bShadow && CasterTest(Bounds);
			if (!bMain && !bCast)
			{
				continue;
			}
			if (Chunk.Current->Count > 0)
			{
				FSpriteChunkDraw Draw;
				Draw.Instances     = Chunk.Current->Buffer.GetGpuAddress();
				Draw.Count         = Chunk.Current->Count;
				Draw.World         = World;
				Draw.Texture       = Texture;
				Draw.Color         = Color;
				Draw.Center        = Bounds.GetCenter();
				Draw.SortLayer     = Layer;
				Draw.OrderInLayer  = Tilemap.OrderInLayer;
				Draw.Blend         = Blend;
				Draw.AlphaCutoff   = Tilemap.AlphaCutoff;
				Draw.bLit          = Tilemap.bLit;
				Draw.Bounds        = Bounds;
				Draw.bShadowStatic = bShadowStatic;
				Draw.PrevWorld     = PrevWorld;
				Draw.bHasPrevWorld = true;
				Draw.bStatic       = bDrawStatic;
				if (bMain)
				{
					Chunks.push_back(Draw);
				}
				if (bCast)
				{
					ShadowChunks.push_back(Draw);
				}
			}
			// 애니메이션 타일: 이번 시간의 프레임 타일로 항목 하나씩 (로컬 사각형 × 월드 — 크기 1, 피벗 0)
			for (const SpriteTiles::FAnimatedCell& Cell : Chunk.Current->Animated)
			{
				const int32            BaseTile   = TileCell::GetTileId(Cell.Cell);
				const FTileDefinition* Definition = Tileset->FindTile(BaseTile);
				const int32            TileId     = Definition != nullptr ? SpriteTiles::SelectAnimationFrame(*Definition, BaseTile, Time) : BaseTile;
				const int32            PrevTileId = Definition != nullptr ? SpriteTiles::SelectAnimationFrame(*Definition, BaseTile, LastCollectTime) : BaseTile;
				const SpriteTiles::FTileQuad Quad = SpriteTiles::ComputeTileQuad(Cell.X, Cell.Y, TileCell::GetFlags(Cell.Cell), CellSize);
				const FSpriteUvRect          Uv   = Tileset->ComputeTileUv(TileId);
				FMatrix4x4 Local = FMatrix4x4::Identity;
				Local.M[0][0] = Quad.AxisX.X; Local.M[0][1] = Quad.AxisX.Y; Local.M[0][2] = Quad.AxisX.Z;
				Local.M[2][0] = Quad.AxisZ.X; Local.M[2][1] = Quad.AxisZ.Y; Local.M[2][2] = Quad.AxisZ.Z;
				Local.M[3][0] = Quad.Origin.X; Local.M[3][1] = Quad.Origin.Y; Local.M[3][2] = Quad.Origin.Z;
				FSpriteDrawItem Item;
				Item.World        = Local * World;
				Item.PrevWorld    = Local * PrevWorld;
				Item.bHasPrevWorld = true;
				Item.bStatic      = bDrawStatic && PrevTileId == TileId; // 프레임이 바뀐 수집만 반응형
				Item.Size         = FVector2(1.0f, 1.0f);
				Item.Pivot        = FVector2(0.0f, 0.0f);
				Item.UVMin        = FVector2(Uv.U0, Uv.V0);
				Item.UVMax        = FVector2(Uv.U1, Uv.V1);
				Item.Texture      = Texture;
				Item.Color        = Color;
				Item.SortLayer    = Layer;
				Item.OrderInLayer = Tilemap.OrderInLayer;
				Item.Blend        = Blend;
				Item.AlphaCutoff  = Tilemap.AlphaCutoff;
				Item.Filter       = Tileset->Filter;
				Item.bLit         = Tilemap.bLit;
				if (bMain)
				{
					Items.push_back(Item);
				}
				if (bCast)
				{
					Item.bCastShadows = true; // 애니메이션 타일 = 항상 동적 캐스터 (프레임마다 바뀜)
					ShadowItems.push_back(Item);
				}
			}
		}
	});

	LastCollectTime = Time;

	// 이번 수집에 안 보인 타일맵(지워짐·타일셋 없음·다른 씬)의 청크 해제
	for (auto It = Tilemaps.begin(); It != Tilemaps.end();)
	{
		if (It->second.LastCollect != CollectIndex)
		{
			for (FChunk& Chunk : It->second.Chunks)
			{
				ReleaseChunk(Chunk);
			}
			It = Tilemaps.erase(It);
		}
		else
		{
			++It;
		}
	}
}

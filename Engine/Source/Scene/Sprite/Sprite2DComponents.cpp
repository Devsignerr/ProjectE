#include "Scene/Sprite/Sprite2DComponents.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/Settings/ProjectSettings.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

// ---- 게으른 해석 -----------------------------------------------------------------------------------------------------

Sprite2DRuntime::FSpriteDisplay Sprite2DRuntime::ResolveSprite(FSpriteComponent& Sprite)
{
	FSpriteRuntime&   Runtime = Sprite.Runtime;
	FSprite2DLibrary& Library = FSprite2DLibrary::Get();
	if (Runtime.FlipbookAtlas != nullptr)
	{
		return { Runtime.FlipbookAtlas, Runtime.FlipbookAtlasPath, Runtime.FlipbookSliceIndex };
	}
	if (Runtime.LoadedPath != Sprite.Sprite || Runtime.Generation != Library.GetGeneration())
	{
		Runtime.Asset      = Library.LoadSprite(Sprite.Sprite);
		Runtime.LoadedPath = Sprite.Sprite;
		Runtime.Generation = Library.GetGeneration();
		Runtime.ResolvedSlice.clear();
		Runtime.SliceIndex = -1;
		if (Runtime.Asset != nullptr)
		{
			Runtime.SliceIndex    = Runtime.Asset->FindSlice(Sprite.Slice);
			Runtime.ResolvedSlice = Sprite.Slice;
		}
	}
	else if (Runtime.Asset != nullptr && Runtime.ResolvedSlice != Sprite.Slice)
	{
		Runtime.SliceIndex    = Runtime.Asset->FindSlice(Sprite.Slice);
		Runtime.ResolvedSlice = Sprite.Slice;
	}
	return { Runtime.Asset, Sprite.Sprite, Runtime.SliceIndex };
}

std::shared_ptr<const FTilesetAsset> Sprite2DRuntime::ResolveTileset(FTilemapComponent& Tilemap)
{
	FTilemapRuntime&  Runtime = Tilemap.Runtime;
	FSprite2DLibrary& Library = FSprite2DLibrary::Get();
	if (Runtime.LoadedPath != Tilemap.Tileset || Runtime.Generation != Library.GetGeneration())
	{
		Runtime.Tileset    = Library.LoadTileset(Tilemap.Tileset);
		Runtime.LoadedPath = Tilemap.Tileset;
		Runtime.Generation = Library.GetGeneration();
	}
	return Runtime.Tileset;
}

const FTilemapData& Sprite2DRuntime::GetTilemapData(FTilemapComponent& Tilemap)
{
	FTilemapRuntime& Runtime = Tilemap.Runtime;
	if (!Runtime.bDecoded || Runtime.DecodedFrom != Tilemap.TileData)
	{
		std::string Error;
		if (!FTilemapData::Decode(Tilemap.TileData, Runtime.Data, &Error))
		{
			E_LOG(LogSprite2D, Error, "[2D] 타일맵 데이터를 읽을 수 없습니다 (타일셋 '{}'): {} — 빈 맵으로 둡니다", Tilemap.Tileset, Error);
		}
		Runtime.DecodedFrom = Tilemap.TileData;
		Runtime.bDecoded    = true;
		Runtime.bDirty      = false; // 밖에서 바뀐 TileData가 이긴다 (대기 중인 편집은 버림)
		++Runtime.Revision;
	}
	return Runtime.Data;
}

void Sprite2DRuntime::CommitTilemapData(FTilemapComponent& Tilemap)
{
	FTilemapRuntime& Runtime = Tilemap.Runtime;
	Tilemap.TileData         = Runtime.Data.Encode();
	Runtime.DecodedFrom      = Tilemap.TileData;
	Runtime.bDecoded         = true;
	Runtime.bDirty           = false;
	++Runtime.CommitCount;
	++Runtime.Revision;
}

void Sprite2DRuntime::BeginTilemapEdit(FTilemapComponent& Tilemap)
{
	GetTilemapData(Tilemap);
	++Tilemap.Runtime.EditDepth;
}

bool Sprite2DRuntime::EndTilemapEdit(FTilemapComponent& Tilemap)
{
	FTilemapRuntime& Runtime = Tilemap.Runtime;
	if (Runtime.EditDepth <= 0)
	{
		return false;
	}
	if (--Runtime.EditDepth == 0 && Runtime.bDirty)
	{
		CommitTilemapData(Tilemap);
	}
	return true;
}

void Sprite2DRuntime::MarkTilemapEdited(FTilemapComponent& Tilemap)
{
	Tilemap.Runtime.bDirty = true;
	++Tilemap.Runtime.Revision;
}

uint32 Sprite2DRuntime::FlushTilemapEdits(FScene& Scene)
{
	uint32 Committed = 0;
	Scene.GetRegistry().View<FTilemapComponent>().Each([&](FEntity Entity, FTilemapComponent& Tilemap) {
		FTilemapRuntime& Runtime = Tilemap.Runtime;
		if (Runtime.EditDepth > 0)
		{
			E_LOG(LogSprite2D, Warning, "[2D] 타일 편집 묶음이 프레임 끝까지 열려 있어 닫습니다 (엔티티 {} — EndTileEdit 누락)", Entity.Index);
			Runtime.EditDepth = 0;
		}
		if (Runtime.bDirty)
		{
			CommitTilemapData(Tilemap);
			++Committed;
		}
	});
	return Committed;
}

// ---- 리플렉션 --------------------------------------------------------------------------------------------------------

void RegisterSprite2DTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();
	// 정렬 레이어: 이름 문자열 (칸 순서를 바꿔도 씬이 깨지지 않게) + 인스펙터는 프로젝트 정렬 레이어 콤보
	const auto            LayerOptions = []() { return FProjectSettings::Get().SortingLayers.GetLayerNames(); };
	constexpr const char* LayerTip     = "정렬 레이어 (프로젝트 설정 → 정렬 레이어). 목록 아래 칸일수록 앞에 그려진다. 비었거나 없는 이름이면 Default";

	Registry.RegisterType<FSpriteComponent>("SpriteComponent", "스프라이트")
		.Property(&FSpriteComponent::Sprite, "Sprite", "스프라이트 아틀라스").AssetFilter(".esprite")
		.Property(&FSpriteComponent::Slice, "Slice", "슬라이스")
		.StringOptionsFor([](const FSpriteComponent& Sprite) {
			// 고른 아틀라스의 슬라이스 이름 (읽기 실패·미선택이면 빈 목록 → 인스펙터는 일반 문자열 칸)
			std::vector<std::string> Names;
			if (const std::shared_ptr<const FSpriteAsset> Asset = Sprite.Sprite.empty() ? nullptr : FSprite2DLibrary::Get().LoadSprite(Sprite.Sprite))
			{
				Names.reserve(Asset->Slices.size());
				for (const FSpriteSlice& Slice : Asset->Slices)
				{
					Names.push_back(Slice.Name);
				}
			}
			return Names;
		})
		.Tooltip("아틀라스 슬라이스 이름. 비면 첫 슬라이스. 같은 엔티티의 플립북이 재생 중이면 플립북 프레임이 우선")
		.Property(&FSpriteComponent::Color, "Color", "색", PF_Color).Tooltip("텍스처에 곱하는 색 (sRGB) + 알파")
		.Property(&FSpriteComponent::bFlipX, "FlipX", "좌우 반전").Tooltip("피벗을 지나는 세로축으로 거울 반사")
		.Property(&FSpriteComponent::bFlipY, "FlipY", "상하 반전")
		.Property(&FSpriteComponent::SortingLayer, "SortingLayer", "정렬 레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.Property(&FSpriteComponent::OrderInLayer, "OrderInLayer", "레이어 안 순서").Tooltip("같은 정렬 레이어 안에서 큰 값이 앞에 그려진다")
		.Property(&FSpriteComponent::bLit, "Lit", "조명 받기").Tooltip("3D 라이트의 영향을 받는다. 끄면 색 그대로 (언릿)")
		.Property(&FSpriteComponent::bCastShadows, "CastShadows", "그림자 드리우기")
		.Property(&FSpriteComponent::Size, "Size", "크기 (cm)").Range(0.0f, 100000.0f, 1.0f)
		.Tooltip("0이면 슬라이스 픽셀 × UnitsPerPixel. 한 축만 주면 비율 유지. 엔티티 스케일은 따로 곱한다")
		.Property(&FSpriteComponent::bVisible, "Visible", "표시")
		.AsComponent();

	Registry.RegisterType<FFlipbookComponent>("FlipbookComponent", "플립북")
		.Property(&FFlipbookComponent::Flipbook, "Flipbook", "플립북").AssetFilter(".eflipbook")
		.Property(&FFlipbookComponent::Speed, "Speed", "속도").Range(-10.0f, 10.0f, 0.01f).Tooltip("음수면 역재생")
		.Property(&FFlipbookComponent::bPlaying, "Playing", "재생")
		.Property(&FFlipbookComponent::StartTime, "StartTime", "시작 시각 (초)").Range(0.0f, 1000.0f, 0.01f)
		.Tooltip("처음 재생(또는 플립북이 바뀌어 다시 시작)할 때의 재생 위치. 같은 플립북 여럿의 박자를 엇갈리게")
		.AsComponent();

	Registry.RegisterType<FTilemapComponent>("TilemapComponent", "타일맵")
		.Property(&FTilemapComponent::Tileset, "Tileset", "타일셋").AssetFilter(".etileset")
		.Property(&FTilemapComponent::CellSize, "CellSize", "셀 크기 (cm)").Range(0.0f, 100000.0f, 1.0f).Tooltip("0이면 타일 픽셀 × UnitsPerPixel")
		.Property(&FTilemapComponent::SortingLayer, "SortingLayer", "정렬 레이어").StringOptions(LayerOptions).Tooltip(LayerTip)
		.Property(&FTilemapComponent::OrderInLayer, "OrderInLayer", "레이어 안 순서").Tooltip("같은 정렬 레이어 안에서 큰 값이 앞에 그려진다")
		.Property(&FTilemapComponent::Color, "Color", "색", PF_Color)
		.Property(&FTilemapComponent::bLit, "Lit", "조명 받기")
		.Property(&FTilemapComponent::bCollision, "Collision", "충돌").Tooltip("타일셋의 충돌 모양으로 2D 정적 바디를 만든다 (Full 타일은 영역 외곽선 — 이음매에 걸리지 않음, OneWay 타일은 윗변 원웨이)")
		.Property(&FTilemapComponent::CollisionLayer, "CollisionLayer", "충돌 레이어")
		.StringOptions([]() { return FProjectSettings::Get().Collision.GetLayerNames(); })
		.Tooltip("타일 충돌의 충돌 레이어 (프로젝트 설정 → 충돌 레이어). 비었거나 없는 이름이면 Default")
		.Property(&FTilemapComponent::Friction, "Friction", "마찰").Range(0.0f, 10.0f, 0.01f)
		.Property(&FTilemapComponent::Restitution, "Restitution", "반발").Range(0.0f, 1.0f, 0.01f)
		.Property(&FTilemapComponent::TileData, "TileData", "타일 데이터", PF_Hidden)
		.AsComponent();
}

#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Sprite/TilemapData.h"

#include <memory>
#include <string>
#include <vector>

struct FSpriteAsset;
struct FFlipbookAsset;
struct FTilesetAsset;

// 2D 컴포넌트 (Phase 56 — 규약은 Scene/Sprite/SpriteAsset.h 머리 주석). 리플렉션 등록 = RegisterSprite2DTypes (RegisterSceneTypes가 부른다).
// 런타임 구조체(…Runtime)는 리플렉션·직렬화 제외이고 복사하면 빈 상태가 된다 (플레이 복제·엔티티 복제가 공유하지 않고 다시 해석하게 —
// FUIComponentRuntime과 같은 관례). 해석은 Sprite2DRuntime 함수가 게으르게 한다.

struct FSpriteRuntime
{
	std::shared_ptr<const FSpriteAsset> Asset;      // Sprite 경로로 읽은 아틀라스
	std::string                         LoadedPath; // Asset을 읽은 경로 (바뀌면 다시)
	uint32                              Generation = 0;
	int32                               SliceIndex = -1; // Slice 이름 해석 결과
	std::string                         ResolvedSlice;   // SliceIndex를 구한 이름

	// 플립북이 표시를 정한다 (FFlipbookSystem이 같은 엔티티에 쓴다). FlipbookAtlas가 있으면 Asset/SliceIndex 대신 이것을 그린다
	std::shared_ptr<const FSpriteAsset> FlipbookAtlas;
	std::string                         FlipbookAtlasPath; // FlipbookAtlas의 Content 기준 경로
	int32                               FlipbookSliceIndex = -1;

	FSpriteRuntime() = default;
	FSpriteRuntime(const FSpriteRuntime&) {}
	FSpriteRuntime& operator=(const FSpriteRuntime&) { return *this; }
	FSpriteRuntime(FSpriteRuntime&&) noexcept            = default;
	FSpriteRuntime& operator=(FSpriteRuntime&&) noexcept = default;
};

// 스프라이트 하나 (엔티티 로컬 X-Z 평면의 사각형, 피벗 = 엔티티 원점)
struct FSpriteComponent
{
	std::string Sprite;                       // .esprite (Content 기준)
	std::string Slice;                        // 슬라이스 이름 (비면 첫 슬라이스)
	FVector4    Color         = FVector4::OneVector; // sRGB + 알파 (곱하기)
	bool        bFlipX        = false;        // 피벗을 지나는 세로축 거울
	bool        bFlipY        = false;
	std::string SortingLayer;                 // 프로젝트 설정 정렬 레이어 이름 (비었거나 없으면 Default)
	int32       OrderInLayer  = 0;            // 같은 레이어 안 순서 (큰 것이 앞)
	bool        bLit          = false;        // 3D 라이트 영향 (끄면 Color 그대로 — 언릿)
	bool        bCastShadows  = false;
	FVector2    Size          = FVector2::ZeroVector; // cm. 0 = 슬라이스 px × UnitsPerPixel (한 축만 주면 비율 유지). 엔티티 스케일은 따로 곱한다
	bool        bVisible      = true;

	FSpriteRuntime Runtime;
};

struct FFlipbookEventRecord
{
	std::string Name;
	int32       Frame = 0;
};

struct FFlipbookRuntime
{
	std::shared_ptr<const FFlipbookAsset> Asset;
	std::shared_ptr<const FSpriteAsset>   Atlas;      // 플립북 Sprite 경로 해석
	std::string                           AtlasPath;  // Atlas의 Content 기준 경로
	std::vector<int32>                    FrameSlices; // 프레임 → 아틀라스 슬라이스 번호 (-1 = 없음)
	std::string                           LoadedPath;
	uint32                                Generation = 0;
	float                                 Time       = 0.0f; // 재생 위치 (초, Loop/PingPong은 주기로 감쌈)
	int32                                 Frame      = -1;
	bool                                  bStarted   = false; // 첫 갱신 전 (시작 프레임 이벤트를 아직 안 냄)
	bool                                  bFinished  = false; // Once가 끝에 닿음 (정방향 끝 / 역방향 0)
	bool                                  bFinishedThisUpdate = false; // 이번 갱신에서 bFinished가 됨 (OnFlipbookFinished 한 번 — 매 갱신 비움)
	std::vector<FFlipbookEventRecord>     PendingEvents; // 이번 갱신의 이벤트 (FFlipbookSystem::Update가 비우고 채움 → 스크립트/게임 모듈이 읽음)

	FFlipbookRuntime() = default;
	FFlipbookRuntime(const FFlipbookRuntime&) {}
	FFlipbookRuntime& operator=(const FFlipbookRuntime&) { return *this; }
	FFlipbookRuntime(FFlipbookRuntime&&) noexcept            = default;
	FFlipbookRuntime& operator=(FFlipbookRuntime&&) noexcept = default;
};

// 플립북 재생 — 같은 엔티티의 FSpriteComponent 표시 슬라이스를 바꾼다 (FFlipbookSystem)
struct FFlipbookComponent
{
	std::string Flipbook;           // .eflipbook (Content 기준)
	float       Speed     = 1.0f;   // 음수 = 역재생
	bool        bPlaying  = true;   // 끄면 현재 프레임에 멈춘다
	float       StartTime = 0.0f;   // 처음(또는 플립북이 바뀌어 다시 시작할 때) 재생 위치 (초)

	FFlipbookRuntime Runtime;
};

struct FTilemapRuntime
{
	FTilemapData                         Data;        // TileData 디코딩 결과
	std::string                          DecodedFrom; // Data를 만든 TileData (바뀌면 다시 디코딩)
	bool                                 bDecoded = false;
	std::shared_ptr<const FTilesetAsset> Tileset;
	std::string                          LoadedPath;
	uint32                               Generation = 0;
	uint32                               Revision   = 0; // Data가 바뀔 때마다 증가 (렌더러/충돌 캐시 무효화용)

	FTilemapRuntime() = default;
	FTilemapRuntime(const FTilemapRuntime&) {}
	FTilemapRuntime& operator=(const FTilemapRuntime&) { return *this; }
	FTilemapRuntime(FTilemapRuntime&&) noexcept            = default;
	FTilemapRuntime& operator=(FTilemapRuntime&&) noexcept = default;
};

// 타일맵 레이어 하나 (여러 층 = 엔티티 여러 개, 유니티 방식). 셀 (0,0) 왼쪽 아래 = 엔티티 원점, +X 오른쪽 +Z 위
struct FTilemapComponent
{
	std::string Tileset;                          // .etileset (Content 기준)
	FVector2    CellSize     = FVector2::ZeroVector; // cm. 0 = 타일 px × UnitsPerPixel
	std::string SortingLayer;
	int32       OrderInLayer = 0;
	FVector4    Color        = FVector4::OneVector; // sRGB + 알파
	bool        bLit         = false;
	bool        bCollision   = true; // 타일 충돌 모양으로 2D 정적 바디를 만든다 (FPhysics2DSystem — 엔티티당 바디 하나)
	std::string CollisionLayer;      // 충돌 레이어 이름 (프로젝트 설정 → 충돌 레이어, 비었거나 없으면 Default)
	float       Friction     = 0.6f; // 2D 콜라이더 기본값과 같음
	float       Restitution  = 0.0f;
	std::string TileData;            // 인코딩된 셀 (FTilemapData::Encode — 인스펙터에 숨김)

	FTilemapRuntime Runtime;
};

// 게으른 해석 (메인 스레드 — FSprite2DLibrary 사용)
namespace Sprite2DRuntime
{
	// 그릴 아틀라스 + 슬라이스 번호. 플립북이 정했으면 그것, 아니면 Sprite/Slice. 없으면 Asset nullptr / Slice -1
	struct FSpriteDisplay
	{
		std::shared_ptr<const FSpriteAsset> Asset;
		std::string                         AssetPath; // Asset의 Content 기준 경로 (텍스처 상대 경로 해석용)
		int32                               SliceIndex = -1;
	};
	FSpriteDisplay ResolveSprite(FSpriteComponent& Sprite);

	// 타일셋 (경로·세대가 바뀌면 다시 읽음)
	std::shared_ptr<const FTilesetAsset> ResolveTileset(FTilemapComponent& Tilemap);
	// 디코딩된 셀 데이터 (TileData가 바뀌었으면 다시 디코딩 — 손상이면 오류 로그 한 번 후 빈 맵)
	const FTilemapData& GetTilemapData(FTilemapComponent& Tilemap);
	// 편집한 Runtime.Data를 TileData로 써 넣는다 (편집기 브러시 → 저장/Undo 대상 문자열)
	void CommitTilemapData(FTilemapComponent& Tilemap);
} // namespace Sprite2DRuntime

// 리플렉션 등록 (RegisterSceneTypes가 부른다 — 여러 번 불러도 한 번)
void RegisterSprite2DTypes();

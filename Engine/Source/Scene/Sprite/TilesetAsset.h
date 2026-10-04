#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Sprite/SpriteAsset.h"

#include <string>
#include <string_view>
#include <vector>

// 타일 충돌 모양 (JSON은 이름 — 끝에만 추가)
enum class ETileCollision : int32
{
	None,
	Full,    // 칸 전체 사각형 (TilemapCollision이 이웃끼리 병합)
	Polygon, // Points 다각형 (타일 로컬 px)
};

const char*    ToString(ETileCollision Collision);
ETileCollision ParseTileCollision(std::string_view Name, bool* bOutValid = nullptr); // 대소문자 무시, 모르면 None

struct FTileAnimFrame
{
	int32 TileId   = 0;
	float Duration = 0.1f; // 초

	bool operator==(const FTileAnimFrame& Other) const = default;
};

// 타일 하나의 추가 정보. 목록에 없는 타일 = 기본값 (충돌 없음, 원웨이 아님, 태그 없음, 애니메이션 없음)
struct FTileDefinition
{
	int32                    Id        = 0; // 타일 번호 = 행 우선 칸 번호 (왼쪽 위 0)
	ETileCollision           Collision = ETileCollision::None;
	std::vector<FVector2>    Points;          // Polygon: 타일 로컬 px (타일 왼쪽 위 원점, 아래로 +), 3개 이상
	bool                     bOneWay = false; // 위에서만 막는 발판 (충돌 모양이 원웨이 목록으로 간다)
	std::vector<std::string> Tags;            // 게임 로직용 (예 "Water", "Ladder")
	std::vector<FTileAnimFrame> Animation;    // 물/횃불처럼 타일 Id를 시간에 따라 바꿔 그린다 (충돌은 이 타일 기준)

	bool IsDefault() const { return Collision == ETileCollision::None && Points.empty() && !bOneWay && Tags.empty() && Animation.empty(); }
	bool HasTag(std::string_view Tag) const;
	bool operator==(const FTileDefinition& Other) const = default;
};

// .etileset — 균일 격자 타일 아틀라스 (JSON)
// { "Version": 1, "Texture": "Tiles.png", "TextureWidth": 64, "TextureHeight": 32, "TileWidth": 16, "TileHeight": 16, "Margin": 0, "Spacing": 0,
//   "Columns": 4, "Rows": 2, "UnitsPerPixel": 1.0, "Filter": "Point",
//   "Tiles": [ { "Id": 1, "Collision": "Full" }, { "Id": 2, "Collision": "Polygon", "Points": [[0,16],[16,16],[16,0]], "OneWay": false,
//              "Tags": ["Slope"] }, { "Id": 5, "Animation": [ { "Tile": 5, "Duration": 0.2 }, { "Tile": 6, "Duration": 0.2 } ] } ] }
//   Columns/Rows는 읽기 편하라고 쓰기만 한다 (읽을 때는 텍스처·타일 크기·Margin·Spacing으로 다시 계산 — SpriteMath::SliceGrid와 같은 식).
//   Tiles는 기본값이 아닌 타일만, Id 오름차순으로 저장한다.
struct FTilesetAsset
{
	static constexpr uint32         Version   = 1;
	static constexpr const wchar_t* Extension = L".etileset";

	std::string   Texture; // 이 파일 폴더 기준 상대 경로
	int32         TextureWidth  = 0;
	int32         TextureHeight = 0;
	int32         TileWidth     = 16; // px
	int32         TileHeight    = 16;
	int32         Margin        = 0;
	int32         Spacing       = 0;
	float         UnitsPerPixel = 1.0f;
	ESpriteFilter Filter        = ESpriteFilter::Point;
	std::vector<FTileDefinition> Tiles; // Id 오름차순

	int32 GetColumns() const;
	int32 GetRows() const;
	int32 GetTileCount() const { return GetColumns() * GetRows(); }
	// 없으면 nullptr (= 기본값 타일)
	const FTileDefinition* FindTile(int32 Id) const;
	// 타일 칸의 이미지 사각형 / UV (범위 밖 Id는 빈 사각형)
	FSpriteSlice  GetTileRect(int32 Id) const;
	FSpriteUvRect ComputeTileUv(int32 Id) const;
	// 타일 칸 크기 (cm) = 타일 px × UnitsPerPixel
	FVector2 GetTileSizeInUnits() const { return FVector2(static_cast<float>(TileWidth) * UnitsPerPixel, static_cast<float>(TileHeight) * UnitsPerPixel); }

	// Tiles를 Id 오름차순 + 기본값 제거 + 같은 Id는 뒤의 것으로 정리 (편집기가 고친 뒤 / 읽을 때)
	void Normalize();

	std::string ToJsonString() const;
	static bool FromJsonString(std::string_view Json, FTilesetAsset& OutAsset, std::vector<std::string>* OutWarnings = nullptr, std::string* OutError = nullptr);

	bool operator==(const FTilesetAsset& Other) const = default;
};

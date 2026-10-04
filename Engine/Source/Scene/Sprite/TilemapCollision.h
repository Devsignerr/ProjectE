#pragma once

#include "Core/Math/Math.h"

#include <vector>

class FTilemapData;
struct FTilesetAsset;

// 타일 충돌 모양 (엔티티 로컬 2D = (X, Z) cm). 물리 바디 생성은 Physics 쪽이 이 결과로 한다 (Phase 56 후속 연결)
struct FTileCollisionBox
{
	FVector2 Min;
	FVector2 Max;

	bool operator==(const FTileCollisionBox& Other) const = default;
};

struct FTileCollisionPolygon
{
	std::vector<FVector2> Points; // 타일셋 점 순서 그대로 (이미지 Y↓ → 로컬 Z↑라 이미지에서 시계 방향 = 로컬 반시계). 반전 셀은 순서를 뒤집어 와인딩 유지

	bool operator==(const FTileCollisionPolygon& Other) const = default;
};

struct FTilemapCollisionShapes
{
	std::vector<FTileCollisionBox>     Boxes;
	std::vector<FTileCollisionPolygon> Polygons;
	std::vector<FTileCollisionBox>     OneWayBoxes;    // bOneWay 타일 (위에서만 막는 발판 — 물리 쪽이 한쪽 충돌로 만든다)
	std::vector<FTileCollisionPolygon> OneWayPolygons;

	bool IsEmpty() const { return Boxes.empty() && Polygons.empty() && OneWayBoxes.empty() && OneWayPolygons.empty(); }
};

// 순수 함수 (테스트: TilemapCollision_*).
//   - 셀의 타일 Id로 타일셋 정의를 찾는다 (애니메이션 타일도 셀에 적힌 Id 기준, 없는 정의/범위 밖 Id = 충돌 없음).
//   - Full 타일: 탐욕 사각형 병합 — 남은 셀 중 (Y, X)가 가장 작은 셀에서 시작해 오른쪽(+X)으로 최대한 늘린 뒤 위(+Y)로 그 폭의 행이
//     모두 차 있는 동안 늘린다 → 사각형 하나, 반복. 결과 순서도 이 순서 (결정적). 원웨이 Full은 따로 같은 식으로 병합.
//   - Polygon 타일: 셀마다 다각형 하나. 타일 px(왼쪽 위 원점, 아래 +) → 정규화(u, v 위) → 셀 플래그 변환(TilemapMath::TransformTileUv)
//     → 셀 사각형. FlipX xor FlipY면 점 순서를 뒤집어 와인딩을 유지한다. 점 3개 미만은 건너뛴다.
//   - CellSize = 셀 크기 cm (0 이하 축은 타일 px × UnitsPerPixel)
namespace TilemapCollision
{
	FVector2                ResolveCellSize(const FTilesetAsset& Tileset, const FVector2& CellSize);
	FTilemapCollisionShapes BuildShapes(const FTilemapData& Data, const FTilesetAsset& Tileset, const FVector2& CellSize);
} // namespace TilemapCollision

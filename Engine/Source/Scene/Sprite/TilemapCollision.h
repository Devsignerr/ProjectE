#pragma once

#include "Core/Math/Math.h"
#include "Scene/Sprite/TilemapData.h"

#include <vector>

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

// Full 영역의 닫힌 외곽선 (로컬 cm). 영역(채운 칸)이 항상 진행 방향 왼쪽 — 바깥 경계는 반시계, 구멍은 시계.
// Box2D 체인은 진행 방향 오른쪽에서만 부딪히므로 이 순서 그대로 닫힌 체인을 만들면 빈 쪽에서만 막힌다 (이음매 걸림 없음)
struct FTileCollisionOutline
{
	std::vector<FVector2> Points; // 꼭짓점만 (공선 점 병합됨, 4점 이상)
	bool                  bHole = false;

	bool operator==(const FTileCollisionOutline& Other) const = default;
};

// 원웨이 Full 타일 윗변 (같은 행에서 이어진 칸은 하나로). Start → End = -X 방향 (위쪽이 진행 방향 오른쪽)
struct FTileCollisionSegment
{
	FVector2 Start;
	FVector2 End;

	bool operator==(const FTileCollisionSegment& Other) const = default;
};

struct FTilemapCollisionShapes
{
	std::vector<FTileCollisionBox>     Boxes;          // Full 병합 상자 (물리는 Outlines를 쓴다 — 다른 용도용으로 유지)
	std::vector<FTileCollisionPolygon> Polygons;
	std::vector<FTileCollisionBox>     OneWayBoxes;    // bOneWay 타일 병합 상자 (물리는 OneWaySegments)
	std::vector<FTileCollisionPolygon> OneWayPolygons;
	std::vector<FTileCollisionOutline> Outlines;       // Full(원웨이 아님) 영역 외곽선 — 물리 체인
	std::vector<FTileCollisionSegment> OneWaySegments; // 원웨이 Full 윗변 — 물리 원웨이 선분

	bool IsEmpty() const { return Boxes.empty() && Polygons.empty() && OneWayBoxes.empty() && OneWayPolygons.empty(); }
};

// 순수 함수 (테스트: TilemapCollision_*).
//   - 셀의 타일 Id로 타일셋 정의를 찾는다 (애니메이션 타일도 셀에 적힌 Id 기준, 없는 정의/범위 밖 Id = 충돌 없음).
//   - Full 타일: 탐욕 사각형 병합 — 남은 셀 중 (Y, X)가 가장 작은 셀에서 시작해 오른쪽(+X)으로 최대한 늘린 뒤 위(+Y)로 그 폭의 행이
//     모두 차 있는 동안 늘린다 → 사각형 하나, 반복. 결과 순서도 이 순서 (결정적). 원웨이 Full은 따로 같은 식으로 병합.
//   - Polygon 타일: 셀마다 다각형 하나. 타일 px(왼쪽 위 원점, 아래 +) → 정규화(u, v 위) → 셀 플래그 변환(TilemapMath::TransformTileUv)
//     → 셀 사각형. FlipX xor FlipY면 점 순서를 뒤집어 와인딩을 유지한다. 점 3개 미만은 건너뛴다.
//   - Full 외곽선(TraceOutlines → Outlines): 채운 칸의 4방향 경계 모서리를 영역이 왼쪽에 오도록 방향을 주고 이어 닫힌 고리로 만든다.
//     시작 = 남은 모서리 중 (시작 꼭짓점 Y, X, 방향 +X/+Y/-X/-Y)이 가장 작은 것, 꼭짓점에서 갈 곳이 여럿이면(대각으로만 닿은 칸) 왼쪽 회전
//     → 직진 → 오른쪽 회전 순. 한 고리가 같은 꼭짓점을 두 번 지나면 그 자리에서 고리를 나눈다(자기 접촉 없는 단순 고리만).
//     공선 점은 지우고, 각 고리는 (Y, X)가 가장 작은 꼭짓점부터, 고리 목록은 점 열 사전순 (결정적). 넓이 < 0 = 구멍.
//     대각으로만 닿은 칸은 서로 다른 고리 (꼭짓점 하나를 공유)
//   - 원웨이 Full 윗변(OneWaySegments): 위 칸이 원웨이 Full이 아닌 원웨이 Full 칸의 윗변을 행마다 X로 이어 붙인 선분, (Y, X) 순
//   - CellSize = 셀 크기 cm (0 이하 축은 타일 px × UnitsPerPixel)
namespace TilemapCollision
{
	FVector2                ResolveCellSize(const FTilesetAsset& Tileset, const FVector2& CellSize);
	FTilemapCollisionShapes BuildShapes(const FTilemapData& Data, const FTilesetAsset& Tileset, const FVector2& CellSize);
	// 채운 칸(중복 허용) → 닫힌 외곽선 (셀 모서리 정수 좌표: 꼭짓점 (x, y) = 셀 (x, y)의 왼쪽 아래). 규칙은 위 주석
	std::vector<std::vector<FTileCoord>> TraceOutlines(const std::vector<FTileCoord>& Cells);
} // namespace TilemapCollision

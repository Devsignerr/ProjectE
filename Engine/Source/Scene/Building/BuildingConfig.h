#pragma once

#include "Core/CoreTypes.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class FDataTable;

// 실내 절차적 생성 (Phase 50 사이드, 아파트식) — 건물 설정 에셋 .ebuilding (JSON, Content 기준 경로, FFileSystem으로 읽음).
//
// 좌표: 건물 로컬 = 엔티티 기준, 왼손 Z-up. 칸 (x, y)는 X(앞) 방향 Width칸 × Y(오른쪽) 방향 Depth칸, 건물 가운데가 원점,
//       층 f의 바닥 높이 = f × FloorHeight. "앞면"(단면 보기에서 벗기는 외벽)은 y = 0 쪽(-Y).
// 형식 (모든 키 선택, 없으면 기본값):
//   { "Version": 1, "CellSize": 100, "Width": 20, "Depth": 12, "Cells": [[x,y], ...](비면 Width×Depth 사각형 전체),
//     "Floors": 5, "FloorHeight": 260, "Seed": 1, "VaryFloors": true, "FixedFloorSeeds": [{ "Floor": 0, "Seed": 7 }],
//     "Core": { "X": 9, "Y": 7, "Width": 3, "Depth": 3 },                 (계단실 — 모든 층 같은 칸, Width 0 = 없음)
//     "Corridor": { "Type": "Center"|"Single"|"None", "Width": 2, "Entrance": true },
//     "Units": { "MinWidth": 4, "MaxWidth": 7 }, "MinRoomSide": 2,
//     "Rooms": [ { "Type": "Living", "Weight": 3, "MinArea": 8, "Required": true, "Chance": 1, "Entry": false, "Hub": true,
//                  "WindowChance": 0.6, "NeedsWindow": true, "Adjacent": ["Entry"], "OpenTo": ["Kitchen"], "Floor": "" } ],
//     "Templates": [ { "Name": "Studio", "Width": 4, "Depth": 5, "Rooms": [ { "Type": "Living", "X": 0, "Y": 0, "Width": 4, "Depth": 3 } ] } ],
//     "FixedUnits": [ { "Floor": 1, "Unit": 0, "Template": "Studio" } ],
//     "Kit": { "Floor": "Prefabs/Interior/Floor.eprefab", "Wall": "...", ... (EBuildingPiece 이름) },
//     "PropTable": "Data/Building/ApartmentProps.etable" }
// 특별한 방 종류 이름: "Corridor"/"Core"가 Rooms에 있으면 복도/계단실의 창 확률·바닥 조각에 쓴다(배정 대상은 아님).
// 모듈 키트 프리팹 규약 (프리팹 루트 = 생성기가 놓는 위치, 칸 크기·층고에 맞게 자식 오프셋/스케일로 맞춘다):
//   Floor/CorridorFloor/Roof: 칸 한 개 가운데, 윗면 Z = 0, 아래로 두께. 생성기가 방 사각형마다 하나를 (칸 수 X, 칸 수 Y, 1)로 늘린다
//   Ceiling: 칸 한 개 가운데, 층 꼭대기(Z = FloorHeight)에 놓이고 아래로 두께 (다음 층 바닥 두께보다 두껍게 — 아래서 보이게)
//   Wall/ExteriorWall/Door/EntranceDoor/Window/Railing: 칸 경계 가운데 바닥(Z = 0), 로컬 Y로 -칸/2..+칸/2, 앞(+X)이 방 안쪽
//   Corner: 격자 꼭짓점 바닥 / Stairs: 칸 한 개 크기 계단(로컬 +X로 오름, 높이 = 층고) — 계단실 빈칸 사각형에 (X칸, Y칸, 1)로 늘린다
//   소품: 바닥 자리 가운데, 앞 = 로컬 +X (벽 소품은 등이 벽)

enum class EBuildingCorridor : uint8
{
	Center, // 가운데 복도, 양쪽에 호실
	Single, // 편복도 (y = 0 쪽), 반대쪽에 호실
	None,   // 복도 없음: 층 전체가 호실 하나 (계단실에서 들어감)
};

enum class EBuildingPiece : uint8
{
	Floor,
	CorridorFloor,
	Ceiling,
	Roof,
	Wall,
	ExteriorWall,
	Door,
	EntranceDoor,
	Window,
	Corner,
	Stairs,
	Railing,
	Prop,
	Count
};

const char* ToString(EBuildingPiece Piece);
bool        TryParseBuildingPiece(std::string_view Text, EBuildingPiece& Out);

struct FBuildingCellRect
{
	int32 X     = 0;
	int32 Y     = 0;
	int32 Width = 0;
	int32 Depth = 0;

	bool IsEmpty() const { return Width <= 0 || Depth <= 0; }
	bool Contains(int32 CellX, int32 CellY) const { return CellX >= X && CellX < X + Width && CellY >= Y && CellY < Y + Depth; }
	bool operator==(const FBuildingCellRect&) const = default;
};

// 방 종류 규칙
struct FBuildingRoomType
{
	std::string              Name;
	float                    Weight       = 1.0f;  // 호실 안 면적 비율
	int32                    MinArea      = 4;     // 칸
	bool                     bRequired    = false; // 호실마다 반드시 (면적이 모자라면 뒤쪽부터 뺀다)
	float                    Chance       = 1.0f;  // 필수가 아니면 들어갈 확률
	bool                     bEntry       = false; // 현관: 복도와 이어지는 호실 입구 방
	bool                     bHub         = false; // 다른 방 문을 우선 이 방에서 낸다 (거실·복도)
	bool                     bNeedsWindow = false; // 외벽에 닿기를 선호
	float                    WindowChance = 0.0f;  // 외벽 칸마다 창이 될 확률
	std::vector<std::string> Adjacent;             // 이 종류 중 하나와 이웃하기를 선호
	std::vector<std::string> OpenTo;               // 이 종류와 맞닿으면 벽 없이 트임
	std::string              FloorPiece;           // 비면 키트 Floor (욕실 타일 등)

	bool operator==(const FBuildingRoomType&) const = default;
};

struct FBuildingTemplateRoom
{
	std::string       Type;
	FBuildingCellRect Rect; // 템플릿 기준 칸

	bool operator==(const FBuildingTemplateRoom&) const = default;
};

// 손으로 지정한 호실 배치. 호실 크기가 다르면 경계를 비례로 옮겨 맞춘다
struct FBuildingUnitTemplate
{
	std::string                        Name;
	int32                              Width = 0; // 복도를 따라
	int32                              Depth = 0; // 복도에서 외벽 쪽
	std::vector<FBuildingTemplateRoom> Rooms;     // 템플릿 사각형을 빈틈없이 덮어야 한다 (빈칸은 이웃 방에 붙음)

	bool operator==(const FBuildingUnitTemplate&) const = default;
};

struct FBuildingFixedUnit
{
	int32       Floor = 0;
	int32       Unit  = 0;
	std::string Template;

	bool operator==(const FBuildingFixedUnit&) const = default;
};

struct FBuildingFloorSeed
{
	int32  Floor = 0;
	uint32 Seed  = 0;

	bool operator==(const FBuildingFloorSeed&) const = default;
};

// 소품 규칙 (데이터 테이블 행 하나 — 구조체 Data/Building/BuildingProp.estruct)
enum class EBuildingPropPlacement : uint8
{
	Wall,   // 등을 벽에
	Corner, // 방 안쪽 모서리
	Center, // 방 가운데
};

struct FBuildingPropRule
{
	std::string            Name;     // 행 이름
	std::string            RoomType; // "Bedroom"
	std::string            Prefab;
	EBuildingPropPlacement Placement = EBuildingPropPlacement::Wall;
	float                  Width     = 100.0f; // 벽을 따라 (cm)
	float                  Depth     = 50.0f;  // 앞뒤 (cm)
	float                  Clearance = 0.0f;   // 앞에 비워 둘 거리 (cm)
	int32                  MinCount  = 1;
	int32                  MaxCount  = 1;
	float                  Chance    = 1.0f;   // 방마다 이 규칙을 쓸 확률
	bool                   bBlocking = true;   // false = 깔개처럼 겹침 검사 제외

	bool operator==(const FBuildingPropRule&) const = default;
};

struct FBuildingConfig
{
	static constexpr const wchar_t* Extension = L".ebuilding";
	static constexpr int32          Version   = 1;

	float                              CellSize    = 100.0f;
	int32                              Width       = 12;
	int32                              Depth       = 10;
	std::vector<std::pair<int32, int32>> Cells;     // 비면 사각형 전체
	int32                              Floors      = 3;
	float                              FloorHeight = 260.0f;
	uint32                             Seed        = 1;
	bool                               bVaryFloors = true;
	std::vector<FBuildingFloorSeed>    FixedFloorSeeds;
	FBuildingCellRect                  Core;
	EBuildingCorridor                  Corridor      = EBuildingCorridor::Center;
	int32                              CorridorWidth = 2;
	bool                               bEntrance     = true; // 1층 복도 끝 외벽에 출입문
	int32                              UnitMinWidth  = 4;
	int32                              UnitMaxWidth  = 7;
	int32                              MinRoomSide   = 2;
	std::vector<FBuildingRoomType>     Rooms;
	std::vector<FBuildingUnitTemplate> Templates;
	std::vector<FBuildingFixedUnit>    FixedUnits;
	std::array<std::string, static_cast<size_t>(EBuildingPiece::Count)> Kit;
	std::string                        PropTable;

	const std::string&           GetKitPiece(EBuildingPiece Piece) const; // 비면 대체 조각(CorridorFloor→Floor, ExteriorWall→Wall, EntranceDoor→Door, Roof→Ceiling)
	const FBuildingRoomType*     FindRoomType(std::string_view Name) const;
	const FBuildingUnitTemplate* FindTemplate(std::string_view Name) const;
	bool                         IsInside(int32 X, int32 Y) const; // 외곽 안 칸인가

	// 크래시 없이: 파싱 실패 = false + 오류, 의미 문제(범위·없는 템플릿)는 OutWarnings
	static bool FromJsonString(const std::string& Text, FBuildingConfig& Out, std::string* OutError = nullptr,
	                           std::vector<std::string>* OutWarnings = nullptr);
	std::string ToJsonString() const;
	bool        operator==(const FBuildingConfig&) const = default;
};

// 데이터 테이블 → 소품 규칙 (필드: RoomType, Prefab, Placement(Wall/Corner/Center), Width, Depth, Clearance, MinCount, MaxCount, Chance, Blocking)
std::vector<FBuildingPropRule> MakeBuildingPropRules(const FDataTable& Table, std::vector<std::string>* OutWarnings = nullptr);

// .ebuilding 경로 캐시 (메인 스레드). 경로는 Content 기준(FPrefabLibrary의 Content 폴더) 또는 절대
class FBuildingLibrary
{
public:
	static FBuildingLibrary& Get();

	std::shared_ptr<const FBuildingConfig> Load(const std::string& AssetPath, std::string* OutError = nullptr);
	void                                   Invalidate();

private:
	std::vector<std::pair<std::wstring, std::shared_ptr<const FBuildingConfig>>> Cache;
};

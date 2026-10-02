#pragma once

#include "Core/Math/Math.h"
#include "Scene/Building/BuildingConfig.h"

#include <string>
#include <vector>

// 실내 절차적 생성 코어 — 순수 로직 (씬/GPU/파일 비의존), 시드 결정적: 같은 설정 + 같은 소품 규칙 + 같은 옵션 → 같은 결과.
// 난수는 자체 SplitMix64(표준 분포 함수 미사용 — 구현마다 결과가 다르다), 순회 순서는 모두 인덱스 순.
//
// 단계 (층마다, 층 시드 = MakeBuildingFloorSeed):
//   1. 뼈대: 외곽 칸 → 계단실(Core, 모든 층 같은 칸) → 복도(Center = 가운데 줄, Single = y 0쪽 줄, None = 없음)
//   2. 호실: 복도 양쪽 띠를 계단실이 끊는 구간으로 나눈 뒤 구간을 너비 [UnitMinWidth, UnitMaxWidth]로 무작위 분할(고정 호실은 템플릿 너비).
//      복도/계단실에 닿지 않는 호실은 이웃 호실에 합친다
//   3. 방: 고정 호실 = 템플릿(경계 비례 사상), 그 밖 = 방 종류 고르기(필수 + 확률) → 사각형 무작위 분할(BSP, 가장 큰 칸을 긴 축으로,
//      변 ≥ MinRoomSide) → 종류 배정(모든 순열 점수: 면적 비율·현관이 복도에 닿음·창 필요 방이 외벽에 닿음·인접 선호, 동점은 시드 순서)
//   4. 문: 계단실↔복도는 트임, 호실 입구 = 현관(없으면 허브/복도에 닿은 방)↔복도 문 하나, 호실 안은 입구에서 최소 신장 트리
//      (허브와 잇는 간선이 싸다). OpenTo 종류끼리는 맞닿은 경계 전체 트임. 1층 복도 끝 외벽에 출입문(Entrance)
//      → 연결성 검사(AreAllRoomsReachable)로 모든 방이 계단실/복도에서 도달 가능함을 보장 (실패하면 경고)
//   5. 창: 외벽 경계마다 방 종류 WindowChance
//   6. 모듈 배치: 방(영역)마다 사각형 분해 → 바닥/천장 조각 늘리기, 경계마다 벽/외벽/문/창(이웃 칸이 정함), 꼭짓점 모서리 기둥(두 축 벽이 만날 때),
//      계단(계단실 빈칸, 꼭대기 층 제외)·난간(위층 계단참↔빈칸), 지붕
//   7. 소품: 방 종류별 규칙 순서대로, 후보(벽 줄/안쪽 모서리/가운데)를 시드로 섞어 놓는다. 겹침 금지(막는 소품끼리 + 앞 비움 영역),
//      문 앞 칸 비움, 방 밖 금지, 놓은 뒤에도 방의 모든 문 앞이 서로 이어지는지(25cm 격자, 장애물 1칸 팽창) 확인
// 단면 보기(SectionFloor ≥ 0): 그 층까지만 만들고 그 층의 천장/지붕과 앞면(y = 0쪽) 외벽을 뺀다.

enum class EBuildingEdge : uint8
{
	None,         // 같은 영역 / 외곽 밖끼리 / 단면으로 뺀 외벽
	Wall,
	Door,
	Window,
	Open,         // 벽 없이 트임
	EntranceDoor, // 건물 출입문 (외벽)
};

struct FBuildingRoom
{
	std::string        Type;      // 방 종류 이름 ("Corridor", "Core" 포함)
	int32              Unit = -1; // 호실 (복도/계단실 -1)
	std::vector<int32> Cells;     // 칸 번호 = y * Width + x (오름차순)
};

// 층 평면 (테스트·디버그용으로 결과에 남는다)
struct FBuildingFloorPlan
{
	int32                      Width = 0;
	int32                      Depth = 0;
	uint32                     Seed  = 0;
	std::vector<int32>         CellRoom;   // 칸 → Rooms 번호 (-1 = 외곽 밖)
	std::vector<FBuildingRoom> Rooms;      // [0] = 복도(있으면), 계단실, 그다음 호실 방들
	std::vector<EBuildingEdge> EdgesX;     // 칸 (x-1, y) | (x, y) 사이 세로 경계: 번호 = y * (Width + 1) + x, x = 0..Width
	std::vector<EBuildingEdge> EdgesY;     // 칸 (x, y-1) | (x, y) 사이 가로 경계: 번호 = y * Width + x, y = 0..Depth
	std::vector<bool>          CoreVoid;   // 칸 → 계단 빈칸 (위층 바닥 없음)
	int32                      UnitCount = 0;
	int32                      CorridorRoom = -1;
	int32                      CoreRoom     = -1;

	int32         GetRoom(int32 X, int32 Y) const; // 밖이면 -1
	EBuildingEdge GetEdgeX(int32 X, int32 Y) const { return EdgesX[static_cast<size_t>(Y * (Width + 1) + X)]; }
	EBuildingEdge GetEdgeY(int32 X, int32 Y) const { return EdgesY[static_cast<size_t>(Y * Width + X)]; }
};

struct FBuildingPlacement
{
	EBuildingPiece Piece = EBuildingPiece::Wall;
	std::string    Asset;                  // 프리팹 (Content 기준)
	FVector3       Position = FVector3::ZeroVector; // 건물 로컬 (cm)
	float          Yaw      = 0.0f;        // 도 (+Yaw = +X에서 +Y 쪽으로)
	FVector3       Scale    = FVector3(1.0f, 1.0f, 1.0f);
	int32          Floor    = 0;
	int32          Unit     = -1;
	int32          Room     = -1;          // 층 평면 Rooms 번호
	std::string    Tag;                    // 방 종류 또는 소품 규칙 이름
	FVector2       FootprintMin = FVector2(0.0f, 0.0f); // 소품만: 건물 로컬 XY 축 정렬 바닥 자리 (cm)
	FVector2       FootprintMax = FVector2(0.0f, 0.0f);
	bool           bBlocking    = false;   // 소품만
};

struct FBuildingGenerateOptions
{
	uint32 Seed         = 1;
	int32  Floors       = 0;  // 0 = 설정 값
	int32  SectionFloor = -1; // 단면 보기 층 (-1 = 없음)
};

struct FBuildingResult
{
	std::vector<FBuildingFloorPlan> Floors;
	std::vector<FBuildingPlacement> Placements;
	std::vector<std::string>        Warnings;

	size_t CountPieces(EBuildingPiece Piece) const;
};

// 층 시드: VaryFloors면 (건물 시드, 층) 해시, 아니면 건물 시드. FixedFloorSeeds가 있으면 그 값
uint32 MakeBuildingFloorSeed(const FBuildingConfig& Config, uint32 Seed, int32 Floor);

FBuildingResult GenerateBuilding(const FBuildingConfig& Config, const std::vector<FBuildingPropRule>& Props, const FBuildingGenerateOptions& Options);

// 연결성: 계단실(없으면 복도, 둘 다 없으면 첫 방) 칸에서 벽이 아닌 경계(None/Open/Door)로 모든 방 칸에 갈 수 있는가. 실패하면 OutProblem
bool AreAllRoomsReachable(const FBuildingFloorPlan& Plan, std::string* OutProblem = nullptr);

// 칸 가운데 (건물 로컬 cm, Z = 0)
FVector3 GetBuildingCellCenter(const FBuildingConfig& Config, int32 X, int32 Y);

#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <string>
#include <vector>

class FScene;
struct FBuildingResult;

// 절차적 건물 엔티티 (Scene/Building). 생성 결과는 이 엔티티 하위에 층/호실 그룹 + 프리팹 인스턴스(일반 엔티티)로 굳는다.
struct FProceduralBuildingComponent
{
	std::string Config;            // .ebuilding (Content 기준)
	int32       Seed         = 1;  // 같은 시드 = 같은 건물
	int32       Floors       = 0;  // 0 = 설정 파일 값
	int32       SectionFloor = -1; // 단면 보기: 이 층(0부터)까지만 만들고 그 층 천장과 앞면(-Y) 외벽을 뺀다 (-1 = 끔)
};

// 생성 그룹 표식 (층 그룹: Unit = -1, 호실 그룹: Unit ≥ 0). 다시 생성하면 그룹 하위는 모두 교체된다
struct FBuildingGroupComponent
{
	int32 Floor = -1;
	int32 Unit  = -1;
};

// 다시 생성해도 남길 엔티티 표식 (그룹 안에서 손으로 고친/추가한 엔티티에 붙인다 — 하위 트리째 유지)
struct FBuildingKeepComponent
{
	bool bKeep = true;
};

void RegisterBuildingTypes();

struct FBuildingApplyStats
{
	int32                    Placements = 0; // 생성기 배치 수
	int32                    Instances  = 0; // 만든 프리팹 인스턴스
	int32                    Kept       = 0; // 유지한 엔티티
	int32                    Failed     = 0; // 프리팹을 못 만든 배치
	double                   GenerateMs = 0.0;
	double                   ApplyMs    = 0.0;
	std::vector<std::string> Warnings;
};

// 다시 생성 보존 정책 (규칙 기준):
//   - 건물 엔티티 바로 아래의 생성 그룹(FBuildingGroupComponent)과 그 하위는 다시 생성할 때 지우고 새로 만든다
//   - 그룹 안에서 FBuildingKeepComponent가 붙은 엔티티는 하위 트리째 남겨 같은 (층, 호실) 새 그룹으로 옮긴다(없으면 층 그룹, 그것도 없으면 건물).
//     그룹은 항상 건물 기준 단위 트랜스폼이므로 옮겨도 월드 위치가 그대로다
//   - 그룹 밖(건물 엔티티 바로 아래의 일반 자식 등)은 건드리지 않는다
// 씬 구조만 바꾼다 — 메시/모델 핸들 해석(FSceneAssetResolver)과 Undo 기록(MarkEdited)은 호출한 쪽이 한다.
class FBuildingSceneBuilder
{
public:
	// Building의 FProceduralBuildingComponent 설정으로 생성 → Apply. 설정/소품 테이블은 FBuildingLibrary/FDataLibrary로 읽는다
	static bool Generate(FScene& Scene, FEntity Building, FBuildingApplyStats* OutStats = nullptr, std::string* OutError = nullptr);
	// 결과를 씬에 굳힌다 (이전 생성물 교체 + 유지 표식 보존)
	static void Apply(FScene& Scene, FEntity Building, const FBuildingResult& Result, FBuildingApplyStats* OutStats = nullptr);
	// 생성물 지우기 (유지 표식 엔티티는 건물 아래로 옮겨 남긴다). 반환 = 지운 그룹 수
	static int32 Clear(FScene& Scene, FEntity Building);
};

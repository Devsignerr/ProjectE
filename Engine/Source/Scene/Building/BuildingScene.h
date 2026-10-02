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

// 생성물 표식 하나로 두 역할 (컴포넌트 수 = 엔진 DLL 내보내기 수라 합쳤다):
//   - 생성 그룹: 생성기가 층 그룹(Unit = -1)·호실 그룹(Unit ≥ 0)에 Floor/Unit을 채우고 bKeep = false로 붙인다
//   - 유지 표식: 손으로 고친/추가한 엔티티에 인스펙터 "컴포넌트 추가"로 붙이면 기본 bKeep = true → 다시 생성해도 하위 트리째 남는다
//     층/호실 그룹 자체에 유지를 켜면 그 층/호실은 고정된다(다음 생성에서 그 자리의 배치를 만들지 않고 그룹을 그대로 둔다)
struct FBuildingPartComponent
{
	int32 Floor = -1; // 그룹만 (0부터)
	int32 Unit  = -1; // 호실 그룹만 (층 그룹 -1)
	bool  bKeep = true;
};

void RegisterBuildingTypes();

struct FBuildingApplyStats
{
	int32                    Placements = 0; // 생성기 배치 수
	int32                    Instances  = 0; // 만든 프리팹 인스턴스
	int32                    Kept       = 0; // 유지한 엔티티/그룹
	int32                    Skipped    = 0; // 고정한 층/호실이라 만들지 않은 배치
	int32                    Failed     = 0; // 프리팹을 못 만든 배치
	double                   GenerateMs = 0.0;
	double                   ApplyMs    = 0.0;
	std::vector<std::string> Warnings;
};

// 다시 생성 보존 정책 (규칙 기준):
//   - 건물 엔티티 바로 아래의 생성 그룹(FBuildingPartComponent, bKeep = false)과 그 하위는 지우고 새로 만든다
//   - 그룹 안에서 bKeep 표식이 붙은 엔티티는 하위 트리째 남겨 같은 (층, 호실) 새 그룹으로 옮긴다(없으면 층 그룹, 그것도 없으면 건물).
//     그룹은 항상 건물 기준 단위 트랜스폼이므로 옮겨도 월드 위치가 그대로다
//   - bKeep을 켠 층/호실 그룹은 그대로 두고 그 자리(층 전체 / 그 층의 그 호실 번호)의 새 배치는 만들지 않는다 (손으로 꾸민 호실 고정).
//     호실 번호는 생성 순서라 시드가 바뀌면 다른 모양의 호실 자리일 수 있다 — 설정의 고정 호실(FixedUnits 템플릿)과 함께 쓰는 것을 권장
//   - 그룹 밖(건물 엔티티 바로 아래의 일반 자식 등)은 건드리지 않는다
// 씬 구조만 바꾼다 — 메시/모델 핸들 해석(FSceneAssetResolver)과 Undo 기록(MarkEdited)은 호출한 쪽이 한다.
class FBuildingSceneBuilder
{
public:
	// Building의 FProceduralBuildingComponent 설정으로 생성 → Apply. 설정/소품 테이블은 FBuildingLibrary/FDataLibrary로 읽는다
	static bool Generate(FScene& Scene, FEntity Building, FBuildingApplyStats* OutStats = nullptr, std::string* OutError = nullptr);
	// 결과를 씬에 굳힌다 (이전 생성물 교체 + 유지 표식 보존)
	static void Apply(FScene& Scene, FEntity Building, const FBuildingResult& Result, FBuildingApplyStats* OutStats = nullptr);
	// 생성물 지우기 (유지 표식 엔티티는 건물 아래로 옮기고, 유지 그룹은 그대로 둔다). 반환 = 지운 그룹 수
	static int32 Clear(FScene& Scene, FEntity Building);
};

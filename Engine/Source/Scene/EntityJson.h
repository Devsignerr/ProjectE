#pragma once

// Scene 모듈 내부용 (nlohmann::json 노출). 씬(.escene)과 프리팹(.eprefab)이 같은 엔티티 목록 직렬화를 쓴다.
// 다른 모듈은 FSceneSerializer / FPrefabLibrary의 문자열 API를 사용한다.
#include "Core/ECS/Entity.h"

#include <json.hpp>

#include <string_view>
#include <vector>

class FScene;
struct FPropertyInfo;
struct FTypeInfo;

struct FEntityJson
{
	// 이름/계층/생성됨 표식: 구조적으로 따로 기록하므로 컴포넌트 목록에서 제외
	static bool IsStructuralType(const FTypeInfo& Type);
	// PF_Transient와 리소스 핸들은 저장하지 않는다
	static bool IsSerializable(const FPropertyInfo& Property);

	// 부모 → 자식 순서로 직렬화 대상 수집 (FTransientComponent 하위 트리 제외)
	static void CollectSubtree(FScene& Scene, FEntity Root, std::vector<FEntity>& Out);

	// 엔티티 목록 → JSON 배열. Entities는 부모가 자식보다 앞선 순서여야 한다.
	// Parent/Entity 프로퍼티는 목록 안 인덱스 (목록 밖이면 -1)
	static nlohmann::json Write(FScene& Scene, const std::vector<FEntity>& Entities);

	// JSON 배열 → 엔티티 생성. Parent가 -1인 항목은 RootParent 아래 (NullEntity면 루트). 반환: 배열 순서의 엔티티
	// 알 수 없는 컴포넌트/프로퍼티는 경고 후 무시. SourceLabel은 로그 표시용
	static std::vector<FEntity> Read(FScene& Scene, const nlohmann::json& Array, FEntity RootParent, std::string_view SourceLabel);
};

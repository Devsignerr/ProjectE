#pragma once

#include "AI/BehaviorTree/Blackboard.h"
#include "AI/BehaviorTree/BehaviorTreeTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 에셋의 노드 하나 (편집 데이터. 실행 상태 없음)
struct FBTNodeDesc
{
	std::string           Type;     // FBehaviorTreeNodeRegistry 키
	uint32                Id = 0;   // 트리 안에서 고유한 고정 ID (편집기/디버그 강조). 0 = 미할당(로드 시 자동 할당)
	std::vector<FBTParam> Params;   // 레지스트리 기본값과 다른 것만 있어도 된다
	std::vector<FBTNodeDesc> Children;   // 컴포지트만
	std::vector<FBTNodeDesc> Decorators; // 위에서 아래 순서 (마지막이 노드에 가장 가깝다)
	std::vector<FBTNodeDesc> Services;

	const FBTParamValue* FindParam(std::string_view Name) const;
	void                 SetParam(std::string_view Name, FBTParamValue Value);
};

// 비헤이비어 트리 에셋 (.ebt, JSON): 블랙보드 키 정의 + 루트 노드
class FBehaviorTreeAsset
{
public:
	static constexpr int32       Version   = 1;
	static constexpr const char* Extension = ".ebt";

	std::vector<FBlackboardKeyDesc> BlackboardKeys;
	std::optional<FBTNodeDesc>      Root;

	std::string ToJsonString() const;
	// 실패하면 false + OutError (문법 오류, 모르는 버전/노드 타입, 구조 오류). 실패 시 이 객체는 바뀌지 않는다
	bool FromJsonString(const std::string& Json, std::string* OutError = nullptr);

	bool SaveToFile(const std::filesystem::path& Path) const;
	bool LoadFromFile(const std::filesystem::path& Path, std::string* OutError = nullptr);

	// 구조 검사: 루트가 컴포지트/태스크, 등록된 타입, 위치별 분류(자식=컴포지트/태스크, 데코레이터, 서비스),
	// 태스크는 자식 없음, SimpleParallel은 자식 1~2개이고 자식 0이 태스크, ID 중복 없음(0 제외), 블랙보드 키 이름 중복 없음
	bool Validate(std::string* OutError = nullptr) const;

	// ID가 0인 노드에 사용하지 않은 ID를 붙인다
	void AssignMissingNodeIds();
	// 사용 중인 최대 ID + 1
	uint32 GetNextNodeId() const;

	FBTNodeDesc*       FindNode(uint32 Id);
	const FBTNodeDesc* FindNode(uint32 Id) const;
};

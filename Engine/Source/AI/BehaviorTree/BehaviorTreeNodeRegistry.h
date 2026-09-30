#pragma once

#include "AI/BehaviorTree/BehaviorTreeNode.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 노드 파라미터 설명 (편집 창이 이 정보로 위젯을 그리고, 실행기가 기본값을 채운다)
struct FBTParamDesc
{
	std::string   Name;
	std::string   DisplayName;
	EPropertyType Type = EPropertyType::Float; // Bool/Int32/Float/String/Vector3
	FBTParamValue Default;
	// String 파라미터의 선택지 (비어 있지 않으면 편집 창이 콤보로 그린다. 예: 중단 모드, 비교 연산)
	std::vector<std::string> Options;
};

using FBTNodeFactory = std::function<std::unique_ptr<FBTNode>()>;

struct FBTNodeInfo
{
	std::string               Name;        // 레지스트리 키 = 에셋의 노드 타입 이름
	std::string               DisplayName;
	EBTNodeCategory           Category = EBTNodeCategory::Task;
	std::vector<FBTParamDesc> Params;
	FBTNodeFactory            Factory;
	std::string               Owner;       // 등록 주체 태그 ("Engine", 게임 모듈 이름 등). UnregisterOwner로 일괄 해제.
	                                       // 비우면 FTypeRegistry의 현재 등록 소유자(게임 모듈 OnLoad 중이면 모듈 이름, 아니면 "Engine")

	const FBTParamDesc* FindParam(std::string_view ParamName) const;
};

// 노드 타입 레지스트리 (엔진 DLL 싱글턴). 엔진 기본 노드는 첫 Get()에서 자동 등록된다 (Owner = "Engine").
// Find가 돌려준 포인터는 그 노드가 해제될 때까지 유효하다.
// 해제해도 이미 만들어진 트리 인스턴스의 노드 객체는 남는다 — 게임 모듈 언로드 전에 트리를 멈춰야 한다
class FBehaviorTreeNodeRegistry
{
public:
	static constexpr const char* EngineOwner = "Engine";

	static FBehaviorTreeNodeRegistry& Get();

	// 이름이 비었거나, 이미 있거나, 팩토리가 없거나, 파라미터 기본값 타입이 선언과 다르면 false.
	// 서비스에는 "Interval"(Float 0.5)/"RandomDeviation"(Float 0)이 없으면 자동으로 추가된다
	bool Register(FBTNodeInfo Info);
	bool Unregister(std::string_view Name);
	// 소유자 태그가 같은 노드를 모두 해제하고 해제한 수를 돌려준다
	int32 UnregisterOwner(std::string_view Owner);

	const FBTNodeInfo*              Find(std::string_view Name) const;
	std::vector<const FBTNodeInfo*> GetAll() const;                          // 이름순
	std::vector<const FBTNodeInfo*> GetByCategory(EBTNodeCategory Category) const;

	// 팩토리로 노드를 만든다. 없거나 팩토리 결과의 분류가 등록 정보와 다르면 nullptr
	std::unique_ptr<FBTNode> Create(std::string_view Name) const;

private:
	FBehaviorTreeNodeRegistry();

	std::map<std::string, std::unique_ptr<FBTNodeInfo>, std::less<>> Nodes;
};

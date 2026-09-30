#pragma once

#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"

#include <string>
#include <vector>

// 비헤이비어 트리 테스트 공용: 실행 기록을 남기는 가짜 태스크/서비스와 에셋 작성 헬퍼
namespace BTTest
{
	constexpr const char* Owner = "BehaviorTreeTests";

	// 가짜 노드 실행 기록 ("exec:A", "tick:A", "abort:A", "svc_begin:S", "svc_tick:S", "svc_end:S")
	std::vector<std::string>& Log();
	// 기록을 쉼표로 이어 붙인다
	std::string JoinLog();

	// 가짜 노드("TestTask", "TestService")를 등록하고 소멸 시 소유자 태그로 해제 + 기록 비움
	struct FScope
	{
		FScope();
		~FScope();
	};

	// TestTask: Ticks = 0이면 OnExecute에서 Result로 끝남, N > 0이면 N번 틱 후 끝남, -1이면 계속 Running
	FBTNodeDesc Task(const std::string& Name, const std::string& Result = "Success", int32 Ticks = 0);
	FBTNodeDesc Service(const std::string& Name, float Interval);
	FBTNodeDesc Composite(const std::string& Type, std::vector<FBTNodeDesc> Children);
	FBTNodeDesc MakeNode(const std::string& Type, std::vector<FBTParam> Params = {});
	FBTNodeDesc BlackboardCondition(const std::string& Key, const std::string& Operation, const std::string& Value, const std::string& AbortMode);
	FBTNodeDesc With(FBTNodeDesc Node, FBTNodeDesc DecoratorOrService);

	FBehaviorTreeAsset MakeAsset(FBTNodeDesc Root, std::vector<FBlackboardKeyDesc> Keys = {});
} // namespace BTTest

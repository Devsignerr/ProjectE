#pragma once

#include "AI/BehaviorTree/BehaviorTreeTypes.h"

#include <string>
#include <vector>

class FBehaviorTreeInstance;

// 비헤이비어 트리 노드 인터페이스 (C++ 노드 작성용. Lua 노드도 이 인터페이스를 구현한다).
// 노드 객체는 트리 인스턴스마다 레지스트리 팩토리로 새로 만들어지므로 멤버에 실행 상태를 두어도 된다.
// 모든 콜백의 Tree는 호출 동안만 유효한 비소유 참조다 (블랙보드, 컨텍스트, 시간, 난수 접근용).
class FBTNode
{
public:
	virtual ~FBTNode() = default;

	virtual EBTNodeCategory GetCategory() const = 0;

	// 인스턴스 생성 직후 한 번. Params = 에셋 값 + 레지스트리 기본값
	virtual void Initialize(const FBTNodeParams& Params) { (void)Params; }
};

// 컴포지트: 자식 실행 순서를 정한다
class FBTCompositeNode : public FBTNode
{
public:
	EBTNodeCategory GetCategory() const final { return EBTNodeCategory::Composite; }

	// 다음에 실행할 자식 인덱스를 돌려준다. PrevChild == -1이면 처음 진입(LastResult 무시).
	// -1을 돌려주면 이 컴포지트가 OutResult로 끝난다. NumChildren == 0이면 처음부터 -1을 돌려줘야 한다
	virtual int32 GetNextChild(int32 PrevChild, EBTStatus LastResult, int32 NumChildren, EBTStatus& OutResult) const = 0;

	// true면 실행기가 SimpleParallel 규칙으로 실행한다 (자식 0 = 주 태스크, 자식 1 = 배경 가지). GetNextChild는 쓰지 않는다
	virtual bool IsSimpleParallel() const { return false; }
};

// 태스크: 잎 노드. OnExecute가 Running을 돌려주면 끝날 때까지 매 틱 OnTick이 불린다 (시작한 틱에는 불리지 않음)
class FBTTaskNode : public FBTNode
{
public:
	EBTNodeCategory GetCategory() const final { return EBTNodeCategory::Task; }

	virtual EBTStatus OnExecute(FBehaviorTreeInstance& Tree) = 0;
	virtual EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime)
	{
		(void)Tree;
		(void)DeltaTime;
		return EBTStatus::Running;
	}
	// Running 중에 중단될 때 (데코레이터 중단, 병렬 배경 중단, Stop)
	virtual void OnAbort(FBehaviorTreeInstance& Tree) { (void)Tree; }
};

// 데코레이터: 붙은 노드(컴포지트/태스크)의 실행 조건과 결과를 바꾼다
class FBTDecoratorNode : public FBTNode
{
public:
	EBTNodeCategory GetCategory() const final { return EBTNodeCategory::Decorator; }

	// 붙은 노드에 진입할 때와 관찰 키가 바뀔 때 평가된다. 거짓이면 붙은 노드는 실행되지 않고 Failure
	virtual bool CalculateCondition(FBehaviorTreeInstance& Tree) { (void)Tree; return true; }

	// 값이 바뀌면 중단 규칙을 평가할 블랙보드 키 (GetAbortMode가 None이면 무시된다)
	virtual void GetObservedKeys(std::vector<std::string>& OutKeys) const { (void)OutKeys; }
	virtual EBTAbortMode GetAbortMode() const { return EBTAbortMode::None; }

	// 붙은 노드가 조건을 통과해 활성화될 때 / 끝나거나 중단될 때
	virtual void OnNodeActivation(FBehaviorTreeInstance& Tree) { (void)Tree; }
	virtual void OnNodeDeactivation(FBehaviorTreeInstance& Tree, EBTStatus Result, bool bAborted)
	{
		(void)Tree;
		(void)Result;
		(void)bAborted;
	}

	// 붙은 노드가 (중단이 아니라) 끝났을 때 결과를 바꾼다 (Inverter/ForceSuccess). 붙은 노드와 가까운(뒤쪽) 데코레이터부터 적용
	virtual EBTStatus ModifyResult(FBehaviorTreeInstance& Tree, EBTStatus Result)
	{
		(void)Tree;
		return Result;
	}

	// 붙은 노드가 끝났을 때 true면 비활성화하지 않고 본문을 다시 실행한다 (Loop). ModifyResult보다 먼저 묻는다
	virtual bool ShouldRepeat(FBehaviorTreeInstance& Tree, EBTStatus Result)
	{
		(void)Tree;
		(void)Result;
		return false;
	}

	// true면 붙은 노드가 활성인 동안 매 틱 TickActive가 불린다 (활성화한 틱 제외)
	virtual bool WantsTick() const { return false; }
	// false를 돌려주면 붙은 가지를 중단하고 부모에 Failure를 전달한다 (TimeLimit)
	virtual bool TickActive(FBehaviorTreeInstance& Tree, float DeltaTime)
	{
		(void)Tree;
		(void)DeltaTime;
		return true;
	}
};

// 서비스: 붙은 노드가 활성인 동안 Interval(± RandomDeviation)마다 OnTick.
// Interval/RandomDeviation은 파라미터 "Interval"/"RandomDeviation"에서 실행기가 채운다 (레지스트리가 서비스에 자동 추가)
class FBTServiceNode : public FBTNode
{
public:
	EBTNodeCategory GetCategory() const final { return EBTNodeCategory::Service; }

	virtual void OnBecomeRelevant(FBehaviorTreeInstance& Tree) { (void)Tree; }
	virtual void OnCeaseRelevant(FBehaviorTreeInstance& Tree) { (void)Tree; }
	// DeltaTime = 직전 OnTick(또는 활성화) 이후 흐른 시간
	virtual void OnTick(FBehaviorTreeInstance& Tree, float DeltaTime)
	{
		(void)Tree;
		(void)DeltaTime;
	}

	float Interval        = 0.5f;
	float RandomDeviation = 0.0f;
};

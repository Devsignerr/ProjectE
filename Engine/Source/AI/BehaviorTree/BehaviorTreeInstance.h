#pragma once

#include "AI/BehaviorTree/Blackboard.h"
#include "AI/BehaviorTree/BehaviorTreeNode.h"

#include <deque>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

class FAISystem;
class FScene;
class FBehaviorTreeAsset;
struct FBTNodeDesc;

// 트리 실행 컨텍스트 (노드가 Tree.GetContext()로 읽는다)
struct FBehaviorTreeContext
{
	FScene*    Scene = nullptr; // 비소유. 인스턴스보다 오래 살아야 한다 (순수 로직 테스트에서는 nullptr)
	FAISystem* AI    = nullptr; // 비소유. 이동/경로 태스크용 (FAISystem이 만든 인스턴스만 설정)
	FEntity    Self;
	uint32  RandomSeed = 0;  // 0이면 비결정적 시드
};

// 비헤이비어 트리 실행기 (UE식 이벤트 기반).
//  - 매 틱 루트부터 재평가하지 않는다. 실행 중(Running) 태스크만 틱하고, 서비스는 활성 가지에서만 간격마다 틱한다
//  - 블랙보드 관찰 키가 바뀌면 대기열에 넣고 다음 Tick 시작에서 중단 규칙을 평가한다 (BehaviorTreeTypes.h의 EBTAbortMode)
//  - 루트가 끝나면 다음 틱에 루트부터 다시 시작한다
//  - 이번 틱에 시작된 태스크/서비스/데코레이터는 그 틱에 틱하지 않는다
//  - 틱(또는 Start)당 노드 방문 예산을 넘으면 남은 진입은 다음 틱으로 미룬다 (즉시 끝나는 노드를 도는 무한 Loop 대비)
// 한 틱의 순서: 지연 진입(루트 재시작/병렬 배경 재시작) → 블랙보드 중단 평가 → 진입 처리 → 서비스/데코레이터/태스크 틱 → 진입 처리
class FBehaviorTreeInstance
{
public:
	static constexpr int32 DefaultNodeVisitBudget = 1000;

	FBehaviorTreeInstance();
	~FBehaviorTreeInstance();
	FBehaviorTreeInstance(const FBehaviorTreeInstance&)            = delete;
	FBehaviorTreeInstance& operator=(const FBehaviorTreeInstance&) = delete;

	// 에셋 구조를 검사하고 노드 객체를 만든다. 블랙보드 키를 에셋 정의로 초기화한다(값은 비워짐).
	// 실행 중이면 먼저 Stop한다. 실패하면 false + OutError
	bool Initialize(const FBehaviorTreeAsset& Asset, const FBehaviorTreeContext& Context = {}, std::string* OutError = nullptr);

	// 루트부터 실행을 시작한다 (첫 진입은 Start 안에서 처리). 이미 실행 중이면 무시
	void Start();
	void Tick(float DeltaTime);
	// 실행 중인 가지를 모두 중단한다 (Running 태스크 OnAbort, 서비스 OnCeaseRelevant)
	void Stop();

	bool IsInitialized() const { return Root != nullptr; }
	bool IsRunning() const { return bRunning; }

	FBlackboard&                GetBlackboard() { return Blackboard; }
	const FBlackboard&          GetBlackboard() const { return Blackboard; }
	const FBehaviorTreeContext& GetContext() const { return Context; }

	// 인스턴스 누적 시간(초)과 틱 번호. Start/Stop으로 되돌리지 않는다 (Cooldown 등이 기준으로 쓴다)
	float  GetTime() const { return Time; }
	uint64 GetTickCount() const { return TickCount; }

	// 인스턴스 난수 [Min, Max] (Context.RandomSeed로 재현 가능)
	float RandomRange(float Min, float Max);

	void  SetNodeVisitBudget(int32 Budget) { NodeVisitBudget = Budget > 0 ? Budget : 1; }
	int32 GetNodeVisitBudget() const { return NodeVisitBudget; }

	// 디버그: 현재 활성인 노드 ID (컴포지트/태스크 + 거기 붙은 데코레이터/서비스), 트리 순서
	std::vector<uint32> GetActiveNodeIds() const;
	bool                IsNodeActive(uint32 Id) const;
	// 마지막으로 루트가 끝난 결과 (아직 없으면 Running)
	EBTStatus GetLastRootResult() const { return LastRootResult; }

private:
	struct FRuntimeNode;

	struct FRuntimeDecorator
	{
		uint32                            Id = 0;
		std::unique_ptr<FBTDecoratorNode> Node;
		std::vector<std::string>          ObservedKeys;
		EBTAbortMode                      AbortMode = EBTAbortMode::None;
		int8                              LastCondition = -1; // -1 = 아직 평가 안 함, 0/1
	};

	struct FRuntimeService
	{
		uint32                          Id = 0;
		std::unique_ptr<FBTServiceNode> Node;
		float                           TimeUntilTick = 0.0f;
		float                           Accumulated   = 0.0f;
	};

	struct FRuntimeNode
	{
		uint32                                     Id       = 0;
		EBTNodeCategory                            Category = EBTNodeCategory::Task;
		std::unique_ptr<FBTNode>                   Node;
		FRuntimeNode*                              Parent     = nullptr; // 비소유 (같은 트리)
		int32                                      ChildIndex = -1;
		std::vector<std::unique_ptr<FRuntimeNode>> Children;
		std::vector<FRuntimeDecorator>             Decorators;
		std::vector<FRuntimeService>               Services;

		// 실행 상태
		bool   bActive      = false;
		bool   bTaskRunning = false;
		int32  CurrentChild = -1;
		uint32 Serial       = 0;  // 활성/비활성/자식 선택이 바뀔 때마다 증가 (대기 중 요청 무효화용)
		uint64 StartTick    = 0;
		bool   bParallel    = false;

		FBTCompositeNode* AsComposite() const { return static_cast<FBTCompositeNode*>(Node.get()); }
		FBTTaskNode*      AsTask() const { return static_cast<FBTTaskNode*>(Node.get()); }
	};

	enum class ERequestType : uint8
	{
		Execute,    // Node에 진입 (데코레이터 조건 평가 → 활성화 → 본문)
		RepeatBody, // 활성 상태인 Node의 본문 재실행 (Loop)
	};

	struct FRequest
	{
		ERequestType  Type = ERequestType::Execute;
		FRuntimeNode* Node = nullptr;
		uint32        Serial = 0; // Execute: 부모 Serial(루트는 RootSerial), RepeatBody: Node Serial
	};

	struct FObservedDecorator
	{
		FRuntimeNode* Node           = nullptr;
		int32         DecoratorIndex = 0;
	};

	std::unique_ptr<FRuntimeNode> BuildNode(const FBTNodeDesc& Desc, FRuntimeNode* Parent, int32 ChildIndex, std::string* OutError);
	void                          CollectObservers(FRuntimeNode& Node);

	void PushExecute(FRuntimeNode& Node);
	void ProcessRequests();
	bool IsRequestValid(const FRequest& Request) const;
	void ExecuteNode(FRuntimeNode& Node);
	void RunBody(FRuntimeNode& Node);
	void SelectChild(FRuntimeNode& Parent, int32 ChildIndex);
	void FinishNode(FRuntimeNode& Node, EBTStatus Result);
	void OnChildFinished(FRuntimeNode& Parent, int32 ChildIndex, EBTStatus Result);
	void OnRootFinished(EBTStatus Result);
	void AbortSubtree(FRuntimeNode& Node);
	void DeactivateNode(FRuntimeNode& Node, EBTStatus Result, bool bAborted);
	// Node를 중단하고 부모(없으면 루트 종료)에 Failure를 전달
	void AbortAndFail(FRuntimeNode& Node);

	void EvaluatePendingAborts();
	void TickActiveNodes(float DeltaTime);
	void ScheduleService(FRuntimeService& Service);

	void CollectActiveIds(const FRuntimeNode& Node, std::vector<uint32>& OutIds) const;

	FBlackboard                   Blackboard;
	FBehaviorTreeContext          Context;
	std::unique_ptr<FRuntimeNode> Root;
	std::vector<FRuntimeNode*>    TickableNodes; // 서비스나 틱 데코레이터가 있는 노드 (트리 순서)
	std::vector<FObservedDecorator> Observers;   // 중단 모드가 있는 관찰 데코레이터 (트리 순서)
	std::vector<FBlackboard::FObserverHandle> ObserverHandles;

	std::deque<FRequest>       Requests;
	std::vector<FRequest>      DeferredRequests;  // 다음 틱 시작에 Requests로 옮긴다
	std::vector<FRuntimeNode*> RunningTasks;      // 시작 순서
	std::vector<std::string>   PendingKeys;       // 다음 틱에 중단 평가할 키 (중복 없음)
	std::vector<std::pair<FRuntimeNode*, uint32>> TaskTickScratch; // 태스크 틱 스냅샷 (틱마다 재할당 방지)

	std::mt19937 Random;
	float        Time            = 0.0f;
	uint64       TickCount       = 0;
	uint32       RootSerial      = 0;
	int32        NodeVisitBudget = DefaultNodeVisitBudget;
	int32        NodeVisits      = 0;
	bool         bRunning        = false;
	bool         bBudgetWarned   = false;
	EBTStatus    LastRootResult  = EBTStatus::Running;
};

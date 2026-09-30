#include "AI/BehaviorTree/BehaviorTreeInstance.h"

#include "AI/AIModule.h"
#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"

#include <algorithm>

namespace
{
	// 레지스트리 선언 파라미터(기본값) 위에 에셋 값을 덮고, 선언되지 않은 에셋 파라미터는 뒤에 붙인다
	FBTNodeParams MergeParams(const FBTNodeInfo& Info, const FBTNodeDesc& Desc)
	{
		FBTNodeParams Params;
		Params.Values.reserve(Info.Params.size() + Desc.Params.size());
		for (const FBTParamDesc& ParamDesc : Info.Params)
		{
			FBTParamValue Value = ParamDesc.Default;
			if (const FBTParamValue* Override = Desc.FindParam(ParamDesc.Name))
			{
				if (std::optional<FBTParamValue> Coerced = BehaviorTreeTypes::CoerceParam(*Override, ParamDesc.Type))
				{
					Value = std::move(*Coerced);
				}
				else
				{
					E_LOG(LogAI, Warning, "노드 '{}'의 파라미터 '{}' 타입이 달라 기본값을 씁니다", Desc.Type, ParamDesc.Name);
				}
			}
			Params.Values.push_back({ ParamDesc.Name, std::move(Value) });
		}
		for (const FBTParam& Param : Desc.Params)
		{
			if (!Info.FindParam(Param.Name))
			{
				Params.Values.push_back(Param);
			}
		}
		return Params;
	}

	template <typename TNode>
	std::unique_ptr<TNode> CastUnique(std::unique_ptr<FBTNode> Node)
	{
		// 분류는 레지스트리 Create가 검사했다
		return std::unique_ptr<TNode>(static_cast<TNode*>(Node.release()));
	}
} // namespace

FBehaviorTreeInstance::FBehaviorTreeInstance()
	: Random(std::random_device{}())
{
}

FBehaviorTreeInstance::~FBehaviorTreeInstance()
{
	Stop();
}

bool FBehaviorTreeInstance::Initialize(const FBehaviorTreeAsset& Asset, const FBehaviorTreeContext& InContext, std::string* OutError)
{
	Stop();
	Root.reset();
	TickableNodes.clear();
	Observers.clear();
	ObserverHandles.clear();
	Requests.clear();
	DeferredRequests.clear();
	RunningTasks.clear();
	PendingKeys.clear();

	Context = InContext;
	Random.seed(Context.RandomSeed != 0 ? Context.RandomSeed : std::random_device{}());
	Blackboard.SetKeys(Asset.BlackboardKeys);

	if (!Asset.Validate(OutError))
	{
		return false;
	}
	if (!Asset.Root)
	{
		if (OutError)
		{
			*OutError = "루트 노드가 없습니다";
		}
		return false;
	}
	std::unique_ptr<FRuntimeNode> NewRoot = BuildNode(*Asset.Root, nullptr, -1, OutError);
	if (!NewRoot)
	{
		return false;
	}
	Root = std::move(NewRoot);
	CollectObservers(*Root);

	// 관찰 키마다 블랙보드 관찰자 하나 (변경 키를 대기열에 넣기만 한다)
	std::vector<std::string> UniqueKeys;
	for (const FObservedDecorator& Observed : Observers)
	{
		for (const std::string& Key : Observed.Node->Decorators[Observed.DecoratorIndex].ObservedKeys)
		{
			if (std::find(UniqueKeys.begin(), UniqueKeys.end(), Key) == UniqueKeys.end())
			{
				UniqueKeys.push_back(Key);
			}
		}
	}
	for (const std::string& Key : UniqueKeys)
	{
		const FBlackboard::FObserverHandle Handle = Blackboard.AddObserver(Key, [this](std::string_view ChangedKey)
		{
			if (bRunning && std::find(PendingKeys.begin(), PendingKeys.end(), ChangedKey) == PendingKeys.end())
			{
				PendingKeys.emplace_back(ChangedKey);
			}
		});
		if (Handle == 0)
		{
			E_LOG(LogAI, Warning, "데코레이터가 관찰하는 블랙보드 키 '{}'가 없습니다", Key);
			continue;
		}
		ObserverHandles.push_back(Handle);
	}
	return true;
}

std::unique_ptr<FBehaviorTreeInstance::FRuntimeNode> FBehaviorTreeInstance::BuildNode(const FBTNodeDesc& Desc, FRuntimeNode* Parent, int32 ChildIndex, std::string* OutError)
{
	const FBehaviorTreeNodeRegistry& Registry = FBehaviorTreeNodeRegistry::Get();
	const auto CreateNode = [&Registry, OutError](const FBTNodeDesc& NodeDesc, std::unique_ptr<FBTNode>& OutNode) -> bool
	{
		const FBTNodeInfo* Info = Registry.Find(NodeDesc.Type);
		OutNode = Registry.Create(NodeDesc.Type);
		if (!Info || !OutNode)
		{
			if (OutError)
			{
				*OutError = std::format("노드 '{}'를 만들 수 없습니다", NodeDesc.Type);
			}
			return false;
		}
		FBTNodeParams Params = MergeParams(*Info, NodeDesc);
		if (OutNode->GetCategory() == EBTNodeCategory::Service)
		{
			FBTServiceNode* Service  = static_cast<FBTServiceNode*>(OutNode.get());
			Service->Interval        = std::max(0.0f, Params.GetFloat("Interval", Service->Interval));
			Service->RandomDeviation = std::max(0.0f, Params.GetFloat("RandomDeviation", Service->RandomDeviation));
		}
		OutNode->Initialize(Params);
		return true;
	};

	auto Runtime        = std::make_unique<FRuntimeNode>();
	Runtime->Id         = Desc.Id;
	Runtime->Parent     = Parent;
	Runtime->ChildIndex = ChildIndex;
	if (!CreateNode(Desc, Runtime->Node))
	{
		return nullptr;
	}
	Runtime->Category = Runtime->Node->GetCategory();
	if (Runtime->Category == EBTNodeCategory::Composite)
	{
		Runtime->bParallel = Runtime->AsComposite()->IsSimpleParallel();
	}

	for (const FBTNodeDesc& DecoratorDesc : Desc.Decorators)
	{
		std::unique_ptr<FBTNode> Node;
		if (!CreateNode(DecoratorDesc, Node))
		{
			return nullptr;
		}
		FRuntimeDecorator Decorator;
		Decorator.Id   = DecoratorDesc.Id;
		Decorator.Node = CastUnique<FBTDecoratorNode>(std::move(Node));
		Decorator.Node->GetObservedKeys(Decorator.ObservedKeys);
		Decorator.AbortMode = Decorator.Node->GetAbortMode();
		Runtime->Decorators.push_back(std::move(Decorator));
	}
	for (const FBTNodeDesc& ServiceDesc : Desc.Services)
	{
		std::unique_ptr<FBTNode> Node;
		if (!CreateNode(ServiceDesc, Node))
		{
			return nullptr;
		}
		FRuntimeService Service;
		Service.Id   = ServiceDesc.Id;
		Service.Node = CastUnique<FBTServiceNode>(std::move(Node));
		Runtime->Services.push_back(std::move(Service));
	}
	for (size_t Index = 0; Index < Desc.Children.size(); ++Index)
	{
		std::unique_ptr<FRuntimeNode> Child = BuildNode(Desc.Children[Index], Runtime.get(), static_cast<int32>(Index), OutError);
		if (!Child)
		{
			return nullptr;
		}
		Runtime->Children.push_back(std::move(Child));
	}
	return Runtime;
}

void FBehaviorTreeInstance::CollectObservers(FRuntimeNode& Node)
{
	bool bTickable = !Node.Services.empty();
	for (size_t Index = 0; Index < Node.Decorators.size(); ++Index)
	{
		const FRuntimeDecorator& Decorator = Node.Decorators[Index];
		bTickable = bTickable || Decorator.Node->WantsTick();
		if (Decorator.AbortMode != EBTAbortMode::None && !Decorator.ObservedKeys.empty())
		{
			Observers.push_back({ &Node, static_cast<int32>(Index) });
		}
	}
	if (bTickable)
	{
		TickableNodes.push_back(&Node);
	}
	for (const std::unique_ptr<FRuntimeNode>& Child : Node.Children)
	{
		CollectObservers(*Child);
	}
}

void FBehaviorTreeInstance::Start()
{
	if (!Root || bRunning)
	{
		return;
	}
	bRunning       = true;
	bBudgetWarned  = false;
	NodeVisits     = 0;
	LastRootResult = EBTStatus::Running;
	PendingKeys.clear();
	for (const FObservedDecorator& Observed : Observers)
	{
		Observed.Node->Decorators[Observed.DecoratorIndex].LastCondition = -1;
	}
	++RootSerial;
	PushExecute(*Root);
	ProcessRequests();
}

void FBehaviorTreeInstance::Stop()
{
	if (!bRunning)
	{
		return;
	}
	if (Root)
	{
		AbortSubtree(*Root);
	}
	Requests.clear();
	DeferredRequests.clear();
	RunningTasks.clear();
	PendingKeys.clear();
	++RootSerial;
	bRunning = false;
}

void FBehaviorTreeInstance::Tick(float DeltaTime)
{
	if (!bRunning)
	{
		return;
	}
	++TickCount;
	Time += DeltaTime;
	NodeVisits = 0;

	for (const FRequest& Request : DeferredRequests)
	{
		Requests.push_back(Request);
	}
	DeferredRequests.clear();

	EvaluatePendingAborts();
	ProcessRequests();
	TickActiveNodes(DeltaTime);
	ProcessRequests();
}

float FBehaviorTreeInstance::RandomRange(float Min, float Max)
{
	if (!(Max > Min))
	{
		return Min;
	}
	std::uniform_real_distribution<float> Distribution(Min, Max);
	return Distribution(Random);
}

void FBehaviorTreeInstance::PushExecute(FRuntimeNode& Node)
{
	Requests.push_back({ ERequestType::Execute, &Node, Node.Parent ? Node.Parent->Serial : RootSerial });
}

bool FBehaviorTreeInstance::IsRequestValid(const FRequest& Request) const
{
	const FRuntimeNode& Node = *Request.Node;
	if (Request.Type == ERequestType::RepeatBody)
	{
		return Node.bActive && Node.Serial == Request.Serial;
	}
	if (Node.bActive)
	{
		return false;
	}
	if (!Node.Parent)
	{
		return Request.Serial == RootSerial;
	}
	const FRuntimeNode& Parent = *Node.Parent;
	if (!Parent.bActive || Parent.Serial != Request.Serial)
	{
		return false;
	}
	return Parent.bParallel || Parent.CurrentChild == Node.ChildIndex;
}

void FBehaviorTreeInstance::ProcessRequests()
{
	while (bRunning && !Requests.empty())
	{
		if (NodeVisits >= NodeVisitBudget)
		{
			if (!bBudgetWarned)
			{
				bBudgetWarned = true;
				E_LOG(LogAI, Warning, "비헤이비어 트리가 틱당 노드 방문 예산({})을 넘어 남은 실행을 다음 틱으로 미룹니다 (즉시 끝나는 무한 Loop?)", NodeVisitBudget);
			}
			return;
		}
		const FRequest Request = Requests.front();
		Requests.pop_front();
		if (!IsRequestValid(Request))
		{
			continue;
		}
		++NodeVisits;
		if (Request.Type == ERequestType::Execute)
		{
			ExecuteNode(*Request.Node);
		}
		else
		{
			RunBody(*Request.Node);
		}
	}
}

void FBehaviorTreeInstance::ExecuteNode(FRuntimeNode& Node)
{
	// 조건은 모두 평가해 마지막 결과를 갱신한다 (관찰 중단의 "결과 변경" 판정 기준)
	bool bPassed = true;
	for (FRuntimeDecorator& Decorator : Node.Decorators)
	{
		const bool bCondition   = Decorator.Node->CalculateCondition(*this);
		Decorator.LastCondition = bCondition ? 1 : 0;
		bPassed                 = bPassed && bCondition;
	}
	if (!bPassed)
	{
		if (Node.Parent)
		{
			OnChildFinished(*Node.Parent, Node.ChildIndex, EBTStatus::Failure);
		}
		else
		{
			OnRootFinished(EBTStatus::Failure);
		}
		return;
	}

	Node.bActive      = true;
	Node.CurrentChild = -1;
	Node.StartTick    = TickCount;
	++Node.Serial;
	for (FRuntimeDecorator& Decorator : Node.Decorators)
	{
		Decorator.Node->OnNodeActivation(*this);
	}
	for (FRuntimeService& Service : Node.Services)
	{
		Service.Accumulated = 0.0f;
		ScheduleService(Service);
		Service.Node->OnBecomeRelevant(*this);
	}
	RunBody(Node);
}

void FBehaviorTreeInstance::RunBody(FRuntimeNode& Node)
{
	if (Node.Category == EBTNodeCategory::Task)
	{
		Node.StartTick         = TickCount;
		const EBTStatus Status = Node.AsTask()->OnExecute(*this);
		if (Status == EBTStatus::Running)
		{
			Node.bTaskRunning = true;
			RunningTasks.push_back(&Node);
		}
		else
		{
			FinishNode(Node, Status);
		}
		return;
	}

	const int32 NumChildren = static_cast<int32>(Node.Children.size());
	if (Node.bParallel)
	{
		// 주 태스크와 배경 가지를 함께 시작한다 (같은 Serial로 두 요청)
		Node.CurrentChild = 0;
		++Node.Serial;
		for (int32 Index = 0; Index < NumChildren; ++Index)
		{
			PushExecute(*Node.Children[Index]);
		}
		return;
	}

	EBTStatus   Result = EBTStatus::Failure;
	const int32 Next   = Node.AsComposite()->GetNextChild(-1, EBTStatus::Failure, NumChildren, Result);
	if (Next < 0 || Next >= NumChildren)
	{
		FinishNode(Node, Result);
		return;
	}
	SelectChild(Node, Next);
}

void FBehaviorTreeInstance::SelectChild(FRuntimeNode& Parent, int32 ChildIndex)
{
	Parent.CurrentChild = ChildIndex;
	++Parent.Serial;
	PushExecute(*Parent.Children[ChildIndex]);
}

void FBehaviorTreeInstance::FinishNode(FRuntimeNode& Node, EBTStatus Result)
{
	if (Node.bTaskRunning)
	{
		Node.bTaskRunning = false;
		std::erase(RunningTasks, &Node);
	}

	// Loop: 비활성화하지 않고 본문을 다시 실행 (대기열을 거치므로 방문 예산에 걸린다)
	for (FRuntimeDecorator& Decorator : Node.Decorators)
	{
		if (Decorator.Node->ShouldRepeat(*this, Result))
		{
			Node.CurrentChild = -1;
			++Node.Serial;
			Requests.push_back({ ERequestType::RepeatBody, &Node, Node.Serial });
			return;
		}
	}

	for (auto It = Node.Decorators.rbegin(); It != Node.Decorators.rend(); ++It)
	{
		Result = It->Node->ModifyResult(*this, Result);
	}
	DeactivateNode(Node, Result, false);

	if (Node.Parent)
	{
		OnChildFinished(*Node.Parent, Node.ChildIndex, Result);
	}
	else
	{
		OnRootFinished(Result);
	}
}

void FBehaviorTreeInstance::OnChildFinished(FRuntimeNode& Parent, int32 ChildIndex, EBTStatus Result)
{
	if (!Parent.bActive)
	{
		return;
	}
	if (Parent.bParallel)
	{
		if (ChildIndex == 0)
		{
			// 주 태스크가 끝나면 배경 가지를 중단하고 주 태스크 결과로 끝난다
			if (Parent.Children.size() > 1)
			{
				AbortSubtree(*Parent.Children[1]);
			}
			FinishNode(Parent, Result);
		}
		else
		{
			// 배경 가지가 먼저 끝나면 다음 틱에 다시 시작한다
			DeferredRequests.push_back({ ERequestType::Execute, Parent.Children[ChildIndex].get(), Parent.Serial });
		}
		return;
	}

	const int32 NumChildren = static_cast<int32>(Parent.Children.size());
	EBTStatus   FinalResult = Result;
	const int32 Next        = Parent.AsComposite()->GetNextChild(ChildIndex, Result, NumChildren, FinalResult);
	if (Next < 0 || Next >= NumChildren)
	{
		FinishNode(Parent, FinalResult);
		return;
	}
	SelectChild(Parent, Next);
}

void FBehaviorTreeInstance::OnRootFinished(EBTStatus Result)
{
	// UE처럼 다음 틱에 루트부터 다시 시작한다
	LastRootResult = Result;
	++RootSerial;
	DeferredRequests.push_back({ ERequestType::Execute, Root.get(), RootSerial });
}

void FBehaviorTreeInstance::DeactivateNode(FRuntimeNode& Node, EBTStatus Result, bool bAborted)
{
	if (Node.bTaskRunning)
	{
		Node.bTaskRunning = false;
		std::erase(RunningTasks, &Node);
		if (bAborted)
		{
			Node.AsTask()->OnAbort(*this);
		}
	}
	for (auto It = Node.Services.rbegin(); It != Node.Services.rend(); ++It)
	{
		It->Node->OnCeaseRelevant(*this);
	}
	for (auto It = Node.Decorators.rbegin(); It != Node.Decorators.rend(); ++It)
	{
		It->Node->OnNodeDeactivation(*this, Result, bAborted);
	}
	Node.bActive      = false;
	Node.CurrentChild = -1;
	++Node.Serial;
}

void FBehaviorTreeInstance::AbortSubtree(FRuntimeNode& Node)
{
	if (!Node.bActive)
	{
		return;
	}
	// 아래에서 위로: 잎 태스크의 OnAbort가 먼저 불린다
	for (auto It = Node.Children.rbegin(); It != Node.Children.rend(); ++It)
	{
		AbortSubtree(**It);
	}
	DeactivateNode(Node, EBTStatus::Failure, true);
}

void FBehaviorTreeInstance::AbortAndFail(FRuntimeNode& Node)
{
	AbortSubtree(Node);
	if (Node.Parent)
	{
		OnChildFinished(*Node.Parent, Node.ChildIndex, EBTStatus::Failure);
	}
	else
	{
		OnRootFinished(EBTStatus::Failure);
	}
}

void FBehaviorTreeInstance::EvaluatePendingAborts()
{
	if (PendingKeys.empty())
	{
		return;
	}
	std::vector<std::string> Keys;
	Keys.swap(PendingKeys);

	for (const FObservedDecorator& Observed : Observers)
	{
		FRuntimeNode&      Node      = *Observed.Node;
		FRuntimeDecorator& Decorator = Node.Decorators[Observed.DecoratorIndex];
		const bool bObserved = std::any_of(Decorator.ObservedKeys.begin(), Decorator.ObservedKeys.end(), [&Keys](const std::string& Key)
		{
			return std::find(Keys.begin(), Keys.end(), Key) != Keys.end();
		});
		if (!bObserved)
		{
			continue;
		}

		// 조건 결과가 바뀐 경우에만 중단 규칙을 적용한다 (UE의 ResultChange 알림)
		const bool bCondition = Decorator.Node->CalculateCondition(*this);
		if (Decorator.LastCondition == (bCondition ? 1 : 0))
		{
			continue;
		}
		Decorator.LastCondition = bCondition ? 1 : 0;

		const bool bSelf  = Decorator.AbortMode == EBTAbortMode::Self || Decorator.AbortMode == EBTAbortMode::Both;
		const bool bLower = Decorator.AbortMode == EBTAbortMode::LowerPriority || Decorator.AbortMode == EBTAbortMode::Both;

		if (bSelf && !bCondition && Node.bActive)
		{
			// Self: 실행 중인 가지 중단 → 부모에 Failure
			AbortAndFail(Node);
			continue;
		}
		if (bLower && bCondition && !Node.bActive)
		{
			// LowerPriority: 부모가 이 노드보다 뒤 자식을 실행 중이면 그 자식을 중단하고 이 노드부터 다시 실행
			FRuntimeNode* Parent = Node.Parent;
			if (Parent && Parent->bActive && !Parent->bParallel && Parent->CurrentChild > Node.ChildIndex)
			{
				AbortSubtree(*Parent->Children[Parent->CurrentChild]);
				SelectChild(*Parent, Node.ChildIndex);
			}
		}
	}
}

void FBehaviorTreeInstance::ScheduleService(FRuntimeService& Service)
{
	const float Deviation = Service.Node->RandomDeviation;
	const float Offset    = Deviation > 0.0f ? RandomRange(-Deviation, Deviation) : 0.0f;
	Service.TimeUntilTick = std::max(0.0f, Service.Node->Interval + Offset);
}

void FBehaviorTreeInstance::TickActiveNodes(float DeltaTime)
{
	// 서비스 → 시간 데코레이터 (이번 틱에 활성화된 노드는 제외)
	for (FRuntimeNode* Node : TickableNodes)
	{
		if (!bRunning)
		{
			return;
		}
		if (!Node->bActive || Node->StartTick >= TickCount)
		{
			continue;
		}
		const uint32 Serial = Node->Serial;
		for (FRuntimeService& Service : Node->Services)
		{
			if (!Node->bActive || Node->Serial != Serial)
			{
				break;
			}
			Service.Accumulated += DeltaTime;
			Service.TimeUntilTick -= DeltaTime;
			if (Service.TimeUntilTick <= 0.0f)
			{
				const float Elapsed = Service.Accumulated;
				Service.Accumulated = 0.0f;
				ScheduleService(Service);
				Service.Node->OnTick(*this, Elapsed);
			}
		}
		for (FRuntimeDecorator& Decorator : Node->Decorators)
		{
			if (!Node->bActive || Node->Serial != Serial)
			{
				break;
			}
			if (Decorator.Node->WantsTick() && !Decorator.Node->TickActive(*this, DeltaTime))
			{
				AbortAndFail(*Node);
				break;
			}
		}
	}

	// 실행 중 태스크 (시작 순서). 틱 도중 끝나거나 중단된 태스크는 건너뛴다
	TaskTickScratch.clear();
	for (FRuntimeNode* Task : RunningTasks)
	{
		TaskTickScratch.emplace_back(Task, Task->Serial);
	}
	for (const auto& [Task, Serial] : TaskTickScratch)
	{
		if (!bRunning)
		{
			return;
		}
		if (!Task->bTaskRunning || Task->Serial != Serial || Task->StartTick >= TickCount)
		{
			continue;
		}
		const EBTStatus Status = Task->AsTask()->OnTick(*this, DeltaTime);
		if (Status != EBTStatus::Running && Task->bTaskRunning && Task->Serial == Serial)
		{
			FinishNode(*Task, Status);
		}
	}
}

std::vector<uint32> FBehaviorTreeInstance::GetActiveNodeIds() const
{
	std::vector<uint32> Ids;
	if (Root)
	{
		CollectActiveIds(*Root, Ids);
	}
	return Ids;
}

bool FBehaviorTreeInstance::IsNodeActive(uint32 Id) const
{
	const std::vector<uint32> Ids = GetActiveNodeIds();
	return std::find(Ids.begin(), Ids.end(), Id) != Ids.end();
}

void FBehaviorTreeInstance::CollectActiveIds(const FRuntimeNode& Node, std::vector<uint32>& OutIds) const
{
	if (!Node.bActive)
	{
		return;
	}
	for (const FRuntimeDecorator& Decorator : Node.Decorators)
	{
		OutIds.push_back(Decorator.Id);
	}
	for (const FRuntimeService& Service : Node.Services)
	{
		OutIds.push_back(Service.Id);
	}
	OutIds.push_back(Node.Id);
	for (const std::unique_ptr<FRuntimeNode>& Child : Node.Children)
	{
		CollectActiveIds(*Child, OutIds);
	}
}

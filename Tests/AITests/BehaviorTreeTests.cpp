#include "BehaviorTreeTestNodes.h"

#include "AI/BehaviorTree/Blackboard.h"
#include "Core/Testing/TestFramework.h"

#include <algorithm>
#include <memory>

using namespace BTTest;

namespace
{
	// 에셋으로 인스턴스를 만들고 초기화 (실패 시 테스트 실패 보고)
	std::unique_ptr<FBehaviorTreeInstance> MakeInstance(const FBehaviorTreeAsset& Asset, uint32 Seed = 1)
	{
		auto        Instance = std::make_unique<FBehaviorTreeInstance>();
		std::string Error;
		FBehaviorTreeContext Context;
		Context.RandomSeed = Seed;
		const bool bInitialized = Instance->Initialize(Asset, Context, &Error);
		E_EXPECT_TRUE(bInitialized);
		E_EXPECT_EQ(Error, std::string());
		return Instance;
	}

	FBlackboardKeyDesc Key(const char* Name, EBlackboardKeyType Type) { return { Name, Type }; }
} // namespace

// ── 블랙보드 ────────────────────────────────────────────────────

E_TEST(Blackboard_TypeCheckedSetGet)
{
	FBlackboard Blackboard;
	Blackboard.SetKeys({ Key("Flag", EBlackboardKeyType::Bool), Key("Count", EBlackboardKeyType::Int),
	                     Key("Speed", EBlackboardKeyType::Float), Key("Target", EBlackboardKeyType::Entity),
	                     Key("Goal", EBlackboardKeyType::Vector), Key("Name", EBlackboardKeyType::String) });

	E_EXPECT_FALSE(Blackboard.IsSet("Count"));
	E_EXPECT_TRUE(Blackboard.SetInt("Count", 3));
	E_EXPECT_TRUE(Blackboard.Get<int32>("Count") == std::optional<int32>(3));

	// 타입 불일치는 거부하고 값은 그대로
	E_EXPECT_FALSE(Blackboard.SetFloat("Count", 1.5f));
	E_EXPECT_FALSE(Blackboard.SetString("Flag", "true"));
	E_EXPECT_TRUE(Blackboard.Get<int32>("Count") == std::optional<int32>(3));
	E_EXPECT_FALSE(Blackboard.Get<float>("Count").has_value());
	E_EXPECT_FALSE(Blackboard.IsSet("Flag"));

	// 없는 키
	E_EXPECT_FALSE(Blackboard.SetInt("Missing", 1));
	E_EXPECT_FALSE(Blackboard.Clear("Missing"));
	E_EXPECT_TRUE(Blackboard.FindKey("Missing") == FBlackboard::InvalidKey);

	E_EXPECT_TRUE(Blackboard.SetEntity("Target", FEntity{ 4, 2 }));
	E_EXPECT_TRUE(Blackboard.Get<FEntity>("Target") == std::optional<FEntity>(FEntity{ 4, 2 }));
	E_EXPECT_TRUE(Blackboard.SetVector("Goal", FVector3(1.0f, 2.0f, 3.0f)));
	E_EXPECT_TRUE(Blackboard.Get<FVector3>("Goal") == std::optional<FVector3>(FVector3(1.0f, 2.0f, 3.0f)));

	E_EXPECT_TRUE(Blackboard.Clear("Count"));
	E_EXPECT_FALSE(Blackboard.IsSet("Count"));
	E_EXPECT_TRUE(Blackboard.GetValue("Count") == nullptr);
}

E_TEST(Blackboard_ObserverCalledOnlyOnChange)
{
	FBlackboard Blackboard;
	Blackboard.SetKeys({ Key("Count", EBlackboardKeyType::Int), Key("Other", EBlackboardKeyType::Int) });

	int32 Calls = 0;
	const FBlackboard::FObserverHandle Handle = Blackboard.AddObserver("Count", [&Calls](std::string_view Changed)
	{
		if (Changed == "Count")
		{
			++Calls;
		}
	});
	E_EXPECT_TRUE(Handle != 0);
	E_EXPECT_TRUE(Blackboard.AddObserver("Missing", [](std::string_view) {}) == 0);

	Blackboard.SetInt("Count", 1); // 설정 안 됨 → 1
	Blackboard.SetInt("Count", 1); // 같은 값: 호출 없음
	Blackboard.SetInt("Other", 5); // 다른 키
	Blackboard.SetInt("Count", 2);
	Blackboard.SetFloat("Count", 3.0f); // 타입 불일치: 호출 없음
	Blackboard.Clear("Count");
	Blackboard.Clear("Count");     // 이미 비어 있음
	E_EXPECT_EQ(Calls, 3);

	Blackboard.RemoveObserver(Handle);
	Blackboard.SetInt("Count", 9);
	E_EXPECT_EQ(Calls, 3);
}

E_TEST(Blackboard_ParseValueByKeyType)
{
	FBlackboardValue Value;
	E_EXPECT_TRUE(BehaviorTreeTypes::ParseBlackboardValue("true", EBlackboardKeyType::Bool, Value));
	E_EXPECT_TRUE(Value == FBlackboardValue(true));
	E_EXPECT_TRUE(BehaviorTreeTypes::ParseBlackboardValue(" -12 ", EBlackboardKeyType::Int, Value));
	E_EXPECT_TRUE(Value == FBlackboardValue(int32(-12)));
	E_EXPECT_TRUE(BehaviorTreeTypes::ParseBlackboardValue("2.5", EBlackboardKeyType::Float, Value));
	E_EXPECT_TRUE(Value == FBlackboardValue(2.5f));
	E_EXPECT_TRUE(BehaviorTreeTypes::ParseBlackboardValue("1, 2.5,-3", EBlackboardKeyType::Vector, Value));
	E_EXPECT_TRUE(Value == FBlackboardValue(FVector3(1.0f, 2.5f, -3.0f)));
	E_EXPECT_FALSE(BehaviorTreeTypes::ParseBlackboardValue("1,2", EBlackboardKeyType::Vector, Value));
	E_EXPECT_FALSE(BehaviorTreeTypes::ParseBlackboardValue("1,2,3,4", EBlackboardKeyType::Vector, Value));
	E_EXPECT_FALSE(BehaviorTreeTypes::ParseBlackboardValue("abc", EBlackboardKeyType::Int, Value));
	E_EXPECT_FALSE(BehaviorTreeTypes::ParseBlackboardValue("1", EBlackboardKeyType::Entity, Value));
}

// ── 레지스트리 ──────────────────────────────────────────────────

E_TEST(BTRegistry_BuiltinNodesRegistered)
{
	const FBehaviorTreeNodeRegistry& Registry = FBehaviorTreeNodeRegistry::Get();
	for (const char* Name : { "Selector", "Sequence", "SimpleParallel", "Blackboard", "Cooldown", "Loop", "TimeLimit",
	                          "Inverter", "ForceSuccess", "Wait", "SetBlackboard", "Log" })
	{
		const FBTNodeInfo* Info = Registry.Find(Name);
		E_EXPECT_TRUE(Info != nullptr);
		if (Info)
		{
			E_EXPECT_EQ(Info->Owner, std::string(FBehaviorTreeNodeRegistry::EngineOwner));
			E_EXPECT_TRUE(Registry.Create(Name) != nullptr);
		}
	}
	E_EXPECT_TRUE(Registry.Find("Selector")->Category == EBTNodeCategory::Composite);
	E_EXPECT_TRUE(Registry.Find("Loop")->Category == EBTNodeCategory::Decorator);
	E_EXPECT_TRUE(Registry.Find("Wait")->Category == EBTNodeCategory::Task);
	E_EXPECT_EQ(Registry.GetByCategory(EBTNodeCategory::Composite).size(), 3u);

	const FBTParamDesc* Operation = Registry.Find("Blackboard")->FindParam("Operation");
	E_EXPECT_TRUE(Operation != nullptr && Operation->Type == EPropertyType::String && Operation->Options.size() == 8u);
}

E_TEST(BTRegistry_UnregisterOwnerRemovesAllOwnedNodes)
{
	FBehaviorTreeNodeRegistry& Registry = FBehaviorTreeNodeRegistry::Get();
	const size_t               Before   = Registry.GetAll().size();
	{
		FScope Scope;
		E_EXPECT_TRUE(Registry.Find("TestTask") != nullptr);
		// 서비스에는 Interval/RandomDeviation이 자동으로 붙는다
		const FBTNodeInfo* ServiceInfo = Registry.Find("TestService");
		E_EXPECT_TRUE(ServiceInfo != nullptr && ServiceInfo->FindParam("Interval") && ServiceInfo->FindParam("RandomDeviation"));

		// 같은 이름 중복 등록, 기본값 타입 불일치는 거부
		FBTNodeInfo Duplicate = *Registry.Find("TestTask");
		E_EXPECT_FALSE(Registry.Register(Duplicate));
		FBTNodeInfo BadParam = Duplicate;
		BadParam.Name        = "BadParamNode";
		BadParam.Params      = { { "X", "X", EPropertyType::Float, FBTParamValue(int32(1)), {} } };
		E_EXPECT_FALSE(Registry.Register(BadParam));

		// 다른 소유자의 노드 두 개
		FBTNodeInfo ModuleA = Duplicate;
		ModuleA.Name        = "ModuleTaskA";
		ModuleA.Owner       = "GameModuleX";
		FBTNodeInfo ModuleB = ModuleA;
		ModuleB.Name        = "ModuleTaskB";
		E_EXPECT_TRUE(Registry.Register(ModuleA));
		E_EXPECT_TRUE(Registry.Register(ModuleB));
		E_EXPECT_EQ(Registry.GetAll().size(), Before + 4);

		E_EXPECT_EQ(Registry.UnregisterOwner("GameModuleX"), 2);
		E_EXPECT_TRUE(Registry.Find("ModuleTaskA") == nullptr);
		E_EXPECT_TRUE(Registry.Find("ModuleTaskB") == nullptr);
		E_EXPECT_TRUE(Registry.Find("TestTask") != nullptr);
		E_EXPECT_EQ(Registry.UnregisterOwner("GameModuleX"), 0);
	}
	// FScope가 테스트 노드를 해제, 엔진 기본 노드는 남는다
	E_EXPECT_TRUE(Registry.Find("TestTask") == nullptr);
	E_EXPECT_TRUE(Registry.Find("Selector") != nullptr);
	E_EXPECT_EQ(Registry.GetAll().size(), Before);
}

// ── 컴포지트 / 태스크 ───────────────────────────────────────────

E_TEST(BT_SequenceStopsOnFirstFailure)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Sequence", { Task("A"), Task("B", "Failure"), Task("C") })));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B"));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Failure);
}

E_TEST(BT_SelectorStopsOnFirstSuccess)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Selector", { Task("A", "Failure"), Task("B"), Task("C") })));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B"));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Success);
}

E_TEST(BT_NestedCompositesPropagateResults)
{
	FScope Scope;
	// Selector[ Sequence[A, B(실패)], Sequence[C, D] ] → A, B 실패로 첫 시퀀스 실패 → C, D
	auto Tree = MakeInstance(MakeAsset(Composite("Selector", {
		Composite("Sequence", { Task("A"), Task("B", "Failure") }),
		Composite("Sequence", { Task("C"), Task("D") }),
	})));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B,exec:C,exec:D"));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Success);
}

E_TEST(BT_RunningTaskSpansTicksAndRootRestarts)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Sequence", { Task("A", "Success", 2), Task("B") })));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A")); // 시작한 틱에는 OnTick 없음
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Running);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,tick:A"));
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,tick:A,tick:A,exec:B"));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Success);
	// 루트가 끝나면 다음 틱에 다시 시작
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,tick:A,tick:A,exec:B,exec:A"));
}

E_TEST(BT_StopAbortsRunningTask)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Sequence", { Task("A", "Success", -1) })));
	Tree->Start();
	E_EXPECT_TRUE(Tree->IsRunning());
	Tree->Stop();
	E_EXPECT_FALSE(Tree->IsRunning());
	E_EXPECT_TRUE(Tree->GetActiveNodeIds().empty());
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,abort:A"));
}

E_TEST(BT_ActiveNodeIdsFollowExecution)
{
	FScope      Scope;
	FBTNodeDesc Root = With(Composite("Sequence", { Task("A", "Success", 1), Task("B", "Success", -1) }), Service("S", 1.0f));
	Root.Id             = 10;
	Root.Services[0].Id = 11;
	Root.Children[0].Id = 20;
	Root.Children[1].Id = 30;
	auto Tree = MakeInstance(MakeAsset(Root));
	Tree->Start();
	E_EXPECT_TRUE(Tree->IsNodeActive(10) && Tree->IsNodeActive(11) && Tree->IsNodeActive(20));
	E_EXPECT_FALSE(Tree->IsNodeActive(30));
	Tree->Tick(0.1f);
	E_EXPECT_TRUE(Tree->GetActiveNodeIds() == std::vector<uint32>({ 11, 10, 30 }));
}

// ── 블랙보드 중단 ───────────────────────────────────────────────

E_TEST(BT_AbortSelfWhenConditionBecomesFalse)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(
		Composite("Selector", {
			With(Task("A", "Success", -1), BlackboardCondition("Flag", "IsSet", "", "Self")),
			Task("B", "Success", -1),
		}),
		{ Key("Flag", EBlackboardKeyType::Bool) }));
	Tree->GetBlackboard().SetBool("Flag", true);
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A"));

	Tree->GetBlackboard().Clear("Flag");
	// 변경은 다음 Tick 시작에서 평가된다
	E_EXPECT_EQ(JoinLog(), std::string("exec:A"));
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,abort:A,exec:B"));

	// Self는 낮은 우선순위를 중단하지 않는다
	Tree->GetBlackboard().SetBool("Flag", true);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,abort:A,exec:B,tick:B"));
}

E_TEST(BT_AbortLowerPriorityWhenConditionBecomesTrue)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(
		Composite("Selector", {
			With(Task("A", "Success", -1), BlackboardCondition("Flag", "IsSet", "", "LowerPriority")),
			Task("B", "Success", -1),
		}),
		{ Key("Flag", EBlackboardKeyType::Bool) }));
	Tree->Start();
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,tick:B"));

	Tree->GetBlackboard().SetBool("Flag", false); // IsSet이므로 false 값도 "설정됨"
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,tick:B,abort:B,exec:A"));

	// LowerPriority는 자기 자신을 중단하지 않는다
	Tree->GetBlackboard().Clear("Flag");
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,tick:B,abort:B,exec:A,tick:A"));
}

E_TEST(BT_AbortBothModes)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(
		Composite("Selector", {
			With(Task("A", "Success", -1), BlackboardCondition("Count", "Greater", "3", "Both")),
			Task("B", "Success", -1),
		}),
		{ Key("Count", EBlackboardKeyType::Int) }));
	Tree->GetBlackboard().SetInt("Count", 1);
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:B"));

	Tree->GetBlackboard().SetInt("Count", 5);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,abort:B,exec:A"));

	// 값은 바뀌었지만 조건 결과는 그대로(참) → 중단 없음
	Tree->GetBlackboard().SetInt("Count", 6);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,abort:B,exec:A,tick:A"));

	Tree->GetBlackboard().SetInt("Count", 2);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,abort:B,exec:A,tick:A,abort:A,exec:B"));
}

E_TEST(BT_AbortModeNoneIgnoresChanges)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(
		Composite("Selector", {
			With(Task("A", "Success", -1), BlackboardCondition("Flag", "IsSet", "", "None")),
			Task("B", "Success", -1),
		}),
		{ Key("Flag", EBlackboardKeyType::Bool) }));
	Tree->Start();
	Tree->GetBlackboard().SetBool("Flag", true);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:B,tick:B"));
}

E_TEST(BT_AbortSelfOnCompositeAbortsWholeBranch)
{
	FScope Scope;
	// 시퀀스 가지 전체(A 완료 후 C 실행 중)가 중단되고 부모 셀렉터가 D로 넘어간다
	auto Tree = MakeInstance(MakeAsset(
		Composite("Selector", {
			With(Composite("Sequence", { Task("A"), Task("C", "Success", -1) }), BlackboardCondition("Alive", "Equals", "true", "Self")),
			Task("D", "Success", -1),
		}),
		{ Key("Alive", EBlackboardKeyType::Bool) }));
	Tree->GetBlackboard().SetBool("Alive", true);
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:C"));
	Tree->GetBlackboard().SetBool("Alive", false);
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:C,abort:C,exec:D"));
}

E_TEST(BT_BlackboardDecoratorOperations)
{
	FScope             Scope;
	FBehaviorTreeAsset Asset = MakeAsset(Task("A"), { Key("I", EBlackboardKeyType::Int), Key("F", EBlackboardKeyType::Float),
	                                                  Key("S", EBlackboardKeyType::String), Key("V", EBlackboardKeyType::Vector) });
	auto Tree = MakeInstance(Asset);
	FBlackboard& Blackboard = Tree->GetBlackboard();
	Blackboard.SetInt("I", 5);
	Blackboard.SetFloat("F", 1.5f);
	Blackboard.SetString("S", "hello");

	struct FCase
	{
		const char* Key;
		const char* Operation;
		const char* Value;
		bool        bExpected;
	};
	const FCase Cases[] = {
		{ "I", "IsSet", "", true },          { "V", "IsSet", "", false },       { "V", "IsNotSet", "", true },
		{ "I", "Equals", "5", true },        { "I", "NotEquals", "5", false },  { "I", "Less", "6", true },
		{ "I", "LessOrEqual", "5", true },   { "I", "Greater", "5", false },    { "I", "GreaterOrEqual", "5", true },
		{ "F", "Greater", "1.25", true },    { "F", "Less", "1.25", false },    { "S", "Equals", "hello", true },
		{ "S", "NotEquals", "hello", false }, { "S", "Less", "z", false },      { "V", "Equals", "0,0,0", false },
	};
	for (const FCase& Case : Cases)
	{
		std::unique_ptr<FBTNode> Node = FBehaviorTreeNodeRegistry::Get().Create("Blackboard");
		FBTNodeParams            Params;
		Params.Values = { { "Key", std::string(Case.Key) }, { "Operation", std::string(Case.Operation) }, { "Value", std::string(Case.Value) } };
		Node->Initialize(Params);
		const bool bResult = static_cast<FBTDecoratorNode*>(Node.get())->CalculateCondition(*Tree);
		if (bResult != Case.bExpected)
		{
			FTestRegistry::ReportFailure(__FILE__, __LINE__, std::format("Blackboard 조건 {} {} '{}' 기대 {}", Case.Key, Case.Operation, Case.Value, Case.bExpected));
		}
	}
}

// ── 데코레이터 ──────────────────────────────────────────────────

E_TEST(BT_InverterAndForceSuccess)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Sequence", {
		With(Task("A", "Failure"), MakeNode("Inverter")),
		With(Task("B", "Failure"), MakeNode("ForceSuccess")),
		With(Task("C", "Success"), MakeNode("Inverter")),
		Task("D"),
	})));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B,exec:C"));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Failure);
}

E_TEST(BT_CooldownBlocksUntilTimePasses)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Selector", {
		With(Task("A"), MakeNode("Cooldown", { { "CooldownTime", 2.0f } })),
		Task("B"),
	})));
	Tree->Start();                // t=0: A
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Tree->Tick(0.5f);         // t=0.5, 1.0, 1.5: B / t=2.0: A
	}
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B,exec:B,exec:B,exec:A"));
}

E_TEST(BT_LoopRepeatsInstantAndRunningNodes)
{
	FScope Scope;
	{
		auto Tree = MakeInstance(MakeAsset(Composite("Sequence", { With(Task("A"), MakeNode("Loop", { { "NumLoops", int32(3) } })), Task("B") })));
		Tree->Start();
		E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:A,exec:A,exec:B"));
	}
	Log().clear();
	{
		// Running 태스크 반복: 끝난 틱에 다시 시작하고, 다시 시작한 틱에는 틱하지 않는다
		auto Tree = MakeInstance(MakeAsset(Composite("Sequence", { With(Task("A", "Success", 1), MakeNode("Loop", { { "NumLoops", int32(2) } })) })));
		Tree->Start();
		Tree->Tick(0.1f);
		Tree->Tick(0.1f);
		E_EXPECT_EQ(JoinLog(), std::string("exec:A,tick:A,exec:A,tick:A"));
		E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Success);
	}
}

E_TEST(BT_InfiniteLoopRespectsVisitBudget)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Sequence", { With(Task("A"), MakeNode("Loop", { { "NumLoops", int32(0) } })) })));
	Tree->SetNodeVisitBudget(50);
	Tree->Start();
	const size_t AfterStart = Log().size();
	E_EXPECT_TRUE(AfterStart > 10u && AfterStart <= 50u);
	Tree->Tick(0.1f);
	const size_t AfterTick = Log().size();
	E_EXPECT_TRUE(AfterTick > AfterStart && AfterTick <= AfterStart + 50u);
	E_EXPECT_TRUE(Tree->IsRunning());
	Tree->Stop();
}

E_TEST(BT_TimeLimitAbortsLongBranch)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Selector", {
		With(Task("A", "Success", -1), MakeNode("TimeLimit", { { "TimeLimit", 1.0f } })),
		Task("B", "Success", -1),
	})));
	Tree->Start();
	Tree->Tick(0.5f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,tick:A"));
	Tree->Tick(0.5f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,tick:A,abort:A,exec:B"));
}

// ── 병렬 / 서비스 ───────────────────────────────────────────────

E_TEST(BT_SimpleParallelRestartsBackgroundAndAbortsItWithMain)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("SimpleParallel", {
		Task("A", "Success", 3),
		Composite("Sequence", { Task("B", "Success", 1) }),
	})));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B"));
	Tree->Tick(0.1f); // B 끝남 → 배경은 다음 틱에 재시작
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B,tick:A,tick:B"));
	Tree->Tick(0.1f);
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B,tick:A,tick:B,exec:B,tick:A"));
	Tree->Tick(0.1f); // A 끝남 → 배경 중단
	E_EXPECT_EQ(JoinLog(), std::string("exec:A,exec:B,tick:A,tick:B,exec:B,tick:A,tick:A,abort:B"));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Success);
}

E_TEST(BT_ServiceTicksAtIntervalOnlyWhileRelevant)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Selector", {
		With(Composite("Sequence", { Task("A", "Failure", 3) }), Service("S", 1.0f)),
		Task("B", "Success", -1),
	})));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("svc_begin:S,exec:A"));
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Tree->Tick(0.5f);
	}
	E_EXPECT_EQ(JoinLog(), std::string("svc_begin:S,exec:A,tick:A,svc_tick:S,tick:A,tick:A,svc_end:S,exec:B,tick:B,tick:B"));
}

// ── 기본 태스크 ─────────────────────────────────────────────────

E_TEST(BT_WaitTaskWithSeededDeviation)
{
	FScope     Scope;
	const auto CountTicksUntilDone = [](float WaitTime, float Deviation, float Step, uint32 Seed)
	{
		Log().clear();
		FBTNodeDesc Wait = MakeNode("Wait", { { "WaitTime", WaitTime }, { "RandomDeviation", Deviation } });
		auto        Tree = MakeInstance(MakeAsset(Composite("Sequence", { Wait, Task("Done") })), Seed);
		Tree->Start();
		int32 Ticks = 0;
		while (Log().empty() && Ticks < 1000)
		{
			Tree->Tick(Step);
			++Ticks;
		}
		return Ticks;
	};

	E_EXPECT_EQ(CountTicksUntilDone(1.0f, 0.0f, 0.25f, 1), 4);

	const int32 First  = CountTicksUntilDone(1.0f, 0.5f, 0.01f, 1234);
	const int32 Second = CountTicksUntilDone(1.0f, 0.5f, 0.01f, 1234);
	E_EXPECT_EQ(First, Second);
	E_EXPECT_TRUE(First >= 50 && First <= 151);
}

E_TEST(BT_SetBlackboardTaskWritesTypedValues)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(
		Composite("Sequence", {
			MakeNode("SetBlackboard", { { "Key", std::string("Count") }, { "Value", std::string("7") } }),
			MakeNode("SetBlackboard", { { "Key", std::string("Goal") }, { "Value", std::string("1, 2, 3") } }),
			MakeNode("SetBlackboard", { { "Key", std::string("Count") }, { "Value", std::string("abc") } }),
			Task("NotReached"),
		}),
		{ Key("Count", EBlackboardKeyType::Int), Key("Goal", EBlackboardKeyType::Vector) }));
	Tree->Start();
	E_EXPECT_TRUE(Tree->GetBlackboard().Get<int32>("Count") == std::optional<int32>(7));
	E_EXPECT_TRUE(Tree->GetBlackboard().Get<FVector3>("Goal") == std::optional<FVector3>(FVector3(1.0f, 2.0f, 3.0f)));
	E_EXPECT_TRUE(Tree->GetLastRootResult() == EBTStatus::Failure); // "abc"는 Int로 해석 불가
	E_EXPECT_TRUE(Log().empty());
}

E_TEST(BT_LogTaskSucceeds)
{
	FScope Scope;
	auto   Tree = MakeInstance(MakeAsset(Composite("Sequence", { MakeNode("Log", { { "Message", std::string("테스트 로그") } }), Task("A") })));
	Tree->Start();
	E_EXPECT_EQ(JoinLog(), std::string("exec:A"));
}

// ── 에셋 ────────────────────────────────────────────────────────

E_TEST(BTAsset_JsonRoundTripPreservesStructure)
{
	FScope      Scope;
	FBTNodeDesc Root = Composite("Selector", {
		With(With(Task("A", "Failure", 2), BlackboardCondition("Target", "IsSet", "", "Both")), Service("S", 0.25f)),
		Composite("Sequence", { MakeNode("Wait", { { "WaitTime", 2.0f } }), Task("B") }),
	});
	Root.Id = 42;
	Root.Children[1].SetParam("Extra", FVector3(1.0f, -2.5f, 3.0f)); // 선언 안 된 파라미터도 보존
	Root.Children[1].SetParam("Flag", true);
	FBehaviorTreeAsset Asset = MakeAsset(Root, { Key("Target", EBlackboardKeyType::Entity), Key("Speed", EBlackboardKeyType::Float) });

	const std::string  Json = Asset.ToJsonString();
	FBehaviorTreeAsset Loaded;
	std::string        Error;
	E_EXPECT_TRUE(Loaded.FromJsonString(Json, &Error));
	E_EXPECT_EQ(Error, std::string());
	E_EXPECT_EQ(Loaded.ToJsonString(), Json);

	E_EXPECT_EQ(Loaded.BlackboardKeys.size(), 2u);
	E_EXPECT_TRUE(Loaded.BlackboardKeys[0].Type == EBlackboardKeyType::Entity);
	E_EXPECT_TRUE(Loaded.Root.has_value());
	if (!Loaded.Root)
	{
		return;
	}
	E_EXPECT_EQ(Loaded.Root->Id, 42u);
	E_EXPECT_EQ(Loaded.Root->Children.size(), 2u);
	const FBTNodeDesc& First = Loaded.Root->Children[0];
	E_EXPECT_EQ(First.Id, Asset.Root->Children[0].Id);
	E_EXPECT_EQ(First.Decorators.size(), 1u);
	E_EXPECT_EQ(First.Services.size(), 1u);
	E_EXPECT_TRUE(First.FindParam("Ticks") && *First.FindParam("Ticks") == FBTParamValue(int32(2)));
	E_EXPECT_TRUE(First.Services[0].FindParam("Interval") && *First.Services[0].FindParam("Interval") == FBTParamValue(0.25f));
	const FBTNodeDesc& Second = Loaded.Root->Children[1];
	E_EXPECT_TRUE(Second.FindParam("Extra") && *Second.FindParam("Extra") == FBTParamValue(FVector3(1.0f, -2.5f, 3.0f)));
	E_EXPECT_TRUE(Second.FindParam("Flag") && *Second.FindParam("Flag") == FBTParamValue(true));
	// 선언이 Float인 파라미터는 JSON에서 정수처럼 보여도 Float로 읽힌다
	E_EXPECT_TRUE(Second.Children[0].FindParam("WaitTime") && *Second.Children[0].FindParam("WaitTime") == FBTParamValue(2.0f));
	E_EXPECT_TRUE(Loaded.FindNode(42) != nullptr);
	E_EXPECT_EQ(Loaded.GetNextNodeId(), Asset.GetNextNodeId());

	// 로드한 에셋으로 실행 가능
	auto Tree = MakeInstance(Loaded);
	Tree->Start();
	E_EXPECT_TRUE(Tree->IsRunning());
}

E_TEST(BTAsset_FloatParamWrittenAsIntegerIsCoerced)
{
	const char* Json = R"({
		"Version": 1,
		"Root": { "Type": "Sequence", "Children": [ { "Type": "Wait", "Params": [ { "Name": "WaitTime", "Value": 3 } ] } ] }
	})";
	FBehaviorTreeAsset Asset;
	E_EXPECT_TRUE(Asset.FromJsonString(Json));
	E_EXPECT_TRUE(Asset.Root && Asset.Root->Children[0].FindParam("WaitTime") && *Asset.Root->Children[0].FindParam("WaitTime") == FBTParamValue(3.0f));
	// Id가 없던 노드에 고유 ID가 붙는다
	E_EXPECT_TRUE(Asset.Root && Asset.Root->Id != 0 && Asset.Root->Children[0].Id != 0 && Asset.Root->Id != Asset.Root->Children[0].Id);
}

E_TEST(BTAsset_LoadFailsForUnknownNodeOrBadStructure)
{
	FBehaviorTreeAsset Asset;
	std::string        Error;

	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Sequence","Children":[{"Type":"NoSuchNode"}]}})", &Error));
	E_EXPECT_TRUE(Error.find("NoSuchNode") != std::string::npos);
	E_EXPECT_FALSE(Asset.Root.has_value()); // 실패하면 바뀌지 않는다

	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":99,"Root":{"Type":"Sequence"}})", &Error));
	E_EXPECT_FALSE(Asset.FromJsonString("{ not json", &Error));
	// 데코레이터를 자식 위치에
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Sequence","Children":[{"Type":"Inverter"}]}})", &Error));
	// 태스크를 데코레이터 위치에
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Sequence","Decorators":[{"Type":"Wait"}]}})", &Error));
	// 태스크에 자식
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Wait","Children":[{"Type":"Wait"}]}})", &Error));
	// SimpleParallel 주 자식은 태스크
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"SimpleParallel","Children":[{"Type":"Sequence"}]}})", &Error));
	// ID 중복
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Sequence","Id":1,"Children":[{"Type":"Wait","Id":1}]}})", &Error));
	// 모르는 블랙보드 키 타입, 키 이름 중복
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Blackboard":[{"Name":"A","Type":"Quat"}]})", &Error));
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Blackboard":[{"Name":"A","Type":"Int"},{"Name":"A","Type":"Bool"}]})", &Error));
	// 선언 타입으로 바꿀 수 없는 파라미터
	E_EXPECT_FALSE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Wait","Params":[{"Name":"WaitTime","Value":"long"}]}})", &Error));

	E_EXPECT_TRUE(Asset.FromJsonString(R"({"Version":1,"Root":{"Type":"Wait"}})", &Error));
}

E_TEST(BTAsset_FileSaveLoad)
{
	FBehaviorTreeAsset Asset = MakeAsset(Composite("Sequence", { MakeNode("Wait", { { "WaitTime", 0.5f } }) }), { Key("Hp", EBlackboardKeyType::Float) });
	const std::filesystem::path Path = FTestRegistry::GetTempDirectory() / (std::string("BehaviorTreeTest") + FBehaviorTreeAsset::Extension);
	E_EXPECT_TRUE(Asset.SaveToFile(Path));

	FBehaviorTreeAsset Loaded;
	E_EXPECT_TRUE(Loaded.LoadFromFile(Path));
	E_EXPECT_EQ(Loaded.ToJsonString(), Asset.ToJsonString());

	std::string Error;
	E_EXPECT_FALSE(Loaded.LoadFromFile(FTestRegistry::GetTempDirectory() / "Missing.ebt", &Error));
	E_EXPECT_FALSE(Error.empty());
	std::filesystem::remove(Path);
}

// 편집기 노드 위치(EditorPosition): 저장/로드 왕복, 없으면 비어 있고 형식이 틀리면 무시(로드는 성공)
E_TEST(BTAsset_EditorPositionRoundTrip)
{
	FBehaviorTreeAsset Asset;
	FBTNodeDesc        Root;
	Root.Type           = "Sequence";
	Root.Id             = 1;
	Root.EditorPosition = FVector2(12.5f, -40.0f);
	FBTNodeDesc Child;
	Child.Type = "Wait";
	Child.Id   = 2;
	Root.Children.push_back(Child);
	Asset.Root = Root;

	FBehaviorTreeAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	E_EXPECT_TRUE(Loaded.Root && Loaded.Root->EditorPosition.has_value());
	E_EXPECT_NEAR(Loaded.Root->EditorPosition->X, 12.5f, 1.0e-4f);
	E_EXPECT_NEAR(Loaded.Root->EditorPosition->Y, -40.0f, 1.0e-4f);
	E_EXPECT_FALSE(Loaded.Root->Children[0].EditorPosition.has_value());

	const std::string  Malformed = R"({"Version":1,"Blackboard":[],"Root":{"Type":"Wait","Id":1,"EditorPosition":"left"}})";
	FBehaviorTreeAsset Tolerant;
	E_EXPECT_TRUE(Tolerant.FromJsonString(Malformed));
	E_EXPECT_FALSE(Tolerant.Root->EditorPosition.has_value());
}

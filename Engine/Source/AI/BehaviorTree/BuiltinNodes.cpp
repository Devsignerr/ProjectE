#include "AI/BehaviorTree/BuiltinNodes.h"

#include "AI/AIModule.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"

#include <algorithm>

namespace
{
	// ── 컴포지트 ────────────────────────────────────────────────

	// 자식을 차례로 실행해 처음 성공한 자식에서 Success, 모두 실패하면 Failure
	class FBTComposite_Selector final : public FBTCompositeNode
	{
	public:
		int32 GetNextChild(int32 PrevChild, EBTStatus LastResult, int32 NumChildren, EBTStatus& OutResult) const override
		{
			if (PrevChild >= 0 && LastResult == EBTStatus::Success)
			{
				OutResult = EBTStatus::Success;
				return -1;
			}
			const int32 Next = PrevChild + 1;
			if (Next >= NumChildren)
			{
				OutResult = EBTStatus::Failure;
				return -1;
			}
			return Next;
		}
	};

	// 자식을 차례로 실행해 처음 실패한 자식에서 Failure, 모두 성공하면 Success (자식이 없으면 Success)
	class FBTComposite_Sequence final : public FBTCompositeNode
	{
	public:
		int32 GetNextChild(int32 PrevChild, EBTStatus LastResult, int32 NumChildren, EBTStatus& OutResult) const override
		{
			if (PrevChild >= 0 && LastResult == EBTStatus::Failure)
			{
				OutResult = EBTStatus::Failure;
				return -1;
			}
			const int32 Next = PrevChild + 1;
			if (Next >= NumChildren)
			{
				OutResult = EBTStatus::Success;
				return -1;
			}
			return Next;
		}
	};

	// 실행 규칙은 FBehaviorTreeInstance가 담당한다
	class FBTComposite_SimpleParallel final : public FBTCompositeNode
	{
	public:
		int32 GetNextChild(int32 PrevChild, EBTStatus LastResult, int32 NumChildren, EBTStatus& OutResult) const override
		{
			(void)PrevChild;
			(void)LastResult;
			(void)NumChildren;
			OutResult = EBTStatus::Failure;
			return -1;
		}
		bool IsSimpleParallel() const override { return true; }
	};

	// ── 데코레이터 ──────────────────────────────────────────────

	enum class EBlackboardOp : uint8
	{
		IsSet,
		IsNotSet,
		Equals,
		NotEquals,
		Less,
		LessOrEqual,
		Greater,
		GreaterOrEqual,
	};

	constexpr const char* BlackboardOpNames[] = { "IsSet", "IsNotSet", "Equals", "NotEquals", "Less", "LessOrEqual", "Greater", "GreaterOrEqual" };

	std::optional<double> ToNumber(const FBlackboardValue& Value)
	{
		if (const int32* Int = std::get_if<int32>(&Value))
		{
			return static_cast<double>(*Int);
		}
		if (const float* Float = std::get_if<float>(&Value))
		{
			return static_cast<double>(*Float);
		}
		return std::nullopt;
	}

	class FBTDecorator_Blackboard final : public FBTDecoratorNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			Key       = Params.GetString("Key");
			ValueText = Params.GetString("Value");

			const std::string OpName = Params.GetString("Operation", "IsSet");
			const auto        It     = std::find_if(std::begin(BlackboardOpNames), std::end(BlackboardOpNames),
			                                        [&OpName](const char* Name) { return OpName == Name; });
			if (It == std::end(BlackboardOpNames))
			{
				E_LOG(LogAI, Warning, "Blackboard 데코레이터: 모르는 연산 '{}' (IsSet으로 처리)", OpName);
			}
			Op = It == std::end(BlackboardOpNames) ? EBlackboardOp::IsSet
			                                       : static_cast<EBlackboardOp>(std::distance(std::begin(BlackboardOpNames), It));

			const std::string ModeName = Params.GetString("AbortMode", "None");
			AbortMode                  = BehaviorTreeTypes::ParseAbortMode(ModeName).value_or(EBTAbortMode::None);
		}

		bool CalculateCondition(FBehaviorTreeInstance& Tree) override
		{
			const FBlackboard&      Blackboard = Tree.GetBlackboard();
			const FBlackboardValue* Value      = Blackboard.GetValue(Key);
			if (Op == EBlackboardOp::IsSet)
			{
				return Value != nullptr;
			}
			if (Op == EBlackboardOp::IsNotSet)
			{
				return Value == nullptr;
			}
			const std::optional<EBlackboardKeyType> Type = Blackboard.GetKeyType(Key);
			FBlackboardValue                        Compare;
			if (!Value || !Type || !BehaviorTreeTypes::ParseBlackboardValue(ValueText, *Type, Compare))
			{
				return false;
			}
			switch (Op)
			{
			case EBlackboardOp::Equals:    return *Value == Compare;
			case EBlackboardOp::NotEquals: return *Value != Compare;
			default: break;
			}
			const std::optional<double> Left  = ToNumber(*Value);
			const std::optional<double> Right = ToNumber(Compare);
			if (!Left || !Right)
			{
				return false;
			}
			switch (Op)
			{
			case EBlackboardOp::Less:           return *Left < *Right;
			case EBlackboardOp::LessOrEqual:    return *Left <= *Right;
			case EBlackboardOp::Greater:        return *Left > *Right;
			case EBlackboardOp::GreaterOrEqual: return *Left >= *Right;
			default:                            return false;
			}
		}

		void GetObservedKeys(std::vector<std::string>& OutKeys) const override
		{
			if (!Key.empty())
			{
				OutKeys.push_back(Key);
			}
		}

		EBTAbortMode GetAbortMode() const override { return AbortMode; }

	private:
		std::string   Key;
		std::string   ValueText;
		EBlackboardOp Op        = EBlackboardOp::IsSet;
		EBTAbortMode  AbortMode = EBTAbortMode::None;
	};

	// 붙은 노드가 끝난 뒤 CooldownTime초 동안 조건 거짓
	class FBTDecorator_Cooldown final : public FBTDecoratorNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { CooldownTime = Params.GetFloat("CooldownTime", 5.0f); }

		bool CalculateCondition(FBehaviorTreeInstance& Tree) override
		{
			return !bHasFinished || Tree.GetTime() - LastFinishTime >= CooldownTime;
		}

		void OnNodeDeactivation(FBehaviorTreeInstance& Tree, EBTStatus Result, bool bAborted) override
		{
			(void)Result;
			(void)bAborted;
			bHasFinished   = true;
			LastFinishTime = Tree.GetTime();
		}

	private:
		float CooldownTime   = 5.0f;
		float LastFinishTime = 0.0f;
		bool  bHasFinished   = false;
	};

	// 붙은 노드를 NumLoops번 실행 (0 이하 = 무한). 결과와 관계없이 반복하고 마지막 결과를 전달한다
	class FBTDecorator_Loop final : public FBTDecoratorNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { NumLoops = Params.GetInt("NumLoops", 3); }

		void OnNodeActivation(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			Count = 0;
		}

		bool ShouldRepeat(FBehaviorTreeInstance& Tree, EBTStatus Result) override
		{
			(void)Tree;
			(void)Result;
			++Count;
			return NumLoops <= 0 || Count < NumLoops;
		}

	private:
		int32 NumLoops = 3;
		int32 Count    = 0;
	};

	// 붙은 노드가 TimeLimit초 넘게 활성이면 중단하고 Failure
	class FBTDecorator_TimeLimit final : public FBTDecoratorNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { TimeLimit = Params.GetFloat("TimeLimit", 5.0f); }

		void OnNodeActivation(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			Elapsed = 0.0f;
		}

		bool WantsTick() const override { return true; }

		bool TickActive(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			(void)Tree;
			Elapsed += DeltaTime;
			return Elapsed < TimeLimit;
		}

	private:
		float TimeLimit = 5.0f;
		float Elapsed   = 0.0f;
	};

	class FBTDecorator_Inverter final : public FBTDecoratorNode
	{
	public:
		EBTStatus ModifyResult(FBehaviorTreeInstance& Tree, EBTStatus Result) override
		{
			(void)Tree;
			return Result == EBTStatus::Success ? EBTStatus::Failure : EBTStatus::Success;
		}
	};

	class FBTDecorator_ForceSuccess final : public FBTDecoratorNode
	{
	public:
		EBTStatus ModifyResult(FBehaviorTreeInstance& Tree, EBTStatus Result) override
		{
			(void)Tree;
			(void)Result;
			return EBTStatus::Success;
		}
	};

	// ── 태스크 ──────────────────────────────────────────────────

	// WaitTime ± RandomDeviation초 기다린 뒤 Success
	class FBTTask_Wait final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			WaitTime        = Params.GetFloat("WaitTime", 1.0f);
			RandomDeviation = std::max(0.0f, Params.GetFloat("RandomDeviation", 0.0f));
		}

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			const float Offset = RandomDeviation > 0.0f ? Tree.RandomRange(-RandomDeviation, RandomDeviation) : 0.0f;
			Remaining          = std::max(0.0f, WaitTime + Offset);
			return Remaining <= 0.0f ? EBTStatus::Success : EBTStatus::Running;
		}

		EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			(void)Tree;
			Remaining -= DeltaTime;
			return Remaining <= 0.0f ? EBTStatus::Success : EBTStatus::Running;
		}

	private:
		float WaitTime        = 1.0f;
		float RandomDeviation = 0.0f;
		float Remaining       = 0.0f;
	};

	// 키에 Value(키 타입으로 해석)를 쓴다. 키가 없거나 해석할 수 없으면 Failure
	class FBTTask_SetBlackboard final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			Key       = Params.GetString("Key");
			ValueText = Params.GetString("Value");
		}

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			FBlackboard&                            Blackboard = Tree.GetBlackboard();
			const std::optional<EBlackboardKeyType> Type       = Blackboard.GetKeyType(Key);
			if (!Type)
			{
				E_LOG(LogAI, Warning, "SetBlackboard: 블랙보드에 없는 키입니다: '{}'", Key);
				return EBTStatus::Failure;
			}
			FBlackboardValue Value;
			if (!BehaviorTreeTypes::ParseBlackboardValue(ValueText, *Type, Value))
			{
				E_LOG(LogAI, Warning, "SetBlackboard: '{}'를 {} 키 '{}' 값으로 해석할 수 없습니다", ValueText, BehaviorTreeTypes::ToString(*Type), Key);
				return EBTStatus::Failure;
			}
			return Blackboard.SetValue(Key, Value) ? EBTStatus::Success : EBTStatus::Failure;
		}

	private:
		std::string Key;
		std::string ValueText;
	};

	class FBTTask_Log final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { Message = Params.GetString("Message"); }

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			E_LOG(LogAI, Display, "[BT] {}", Message);
			return EBTStatus::Success;
		}

	private:
		std::string Message;
	};

	// ── 등록 ────────────────────────────────────────────────────

	template <typename TNode>
	FBTNodeInfo MakeInfo(const char* Name, const char* DisplayName, EBTNodeCategory Category, std::vector<FBTParamDesc> Params = {})
	{
		FBTNodeInfo Info;
		Info.Name        = Name;
		Info.DisplayName = DisplayName;
		Info.Category    = Category;
		Info.Params      = std::move(Params);
		Info.Factory     = []() -> std::unique_ptr<FBTNode> { return std::make_unique<TNode>(); };
		Info.Owner       = FBehaviorTreeNodeRegistry::EngineOwner;
		return Info;
	}

	FBTParamDesc FloatParam(const char* Name, const char* DisplayName, float Default)
	{
		return { Name, DisplayName, EPropertyType::Float, FBTParamValue(Default), {} };
	}

	FBTParamDesc IntParam(const char* Name, const char* DisplayName, int32 Default)
	{
		return { Name, DisplayName, EPropertyType::Int32, FBTParamValue(Default), {} };
	}

	FBTParamDesc StringParam(const char* Name, const char* DisplayName, const char* Default, std::vector<std::string> Options = {})
	{
		return { Name, DisplayName, EPropertyType::String, FBTParamValue(std::string(Default)), std::move(Options) };
	}
} // namespace

void RegisterBuiltinBehaviorTreeNodes(FBehaviorTreeNodeRegistry& Registry)
{
	Registry.Register(MakeInfo<FBTComposite_Selector>("Selector", "선택자 (Selector)", EBTNodeCategory::Composite));
	Registry.Register(MakeInfo<FBTComposite_Sequence>("Sequence", "시퀀스 (Sequence)", EBTNodeCategory::Composite));
	Registry.Register(MakeInfo<FBTComposite_SimpleParallel>("SimpleParallel", "단순 병렬 (Simple Parallel)", EBTNodeCategory::Composite));

	std::vector<std::string> OpOptions(std::begin(BlackboardOpNames), std::end(BlackboardOpNames));
	Registry.Register(MakeInfo<FBTDecorator_Blackboard>("Blackboard", "블랙보드 조건", EBTNodeCategory::Decorator,
		{
			StringParam("Key", "키", ""),
			StringParam("Operation", "연산", "IsSet", std::move(OpOptions)),
			StringParam("Value", "비교값", ""),
			StringParam("AbortMode", "중단 모드", "None", { "None", "Self", "LowerPriority", "Both" }),
		}));
	Registry.Register(MakeInfo<FBTDecorator_Cooldown>("Cooldown", "쿨다운", EBTNodeCategory::Decorator,
		{ FloatParam("CooldownTime", "쿨다운(초)", 5.0f) }));
	Registry.Register(MakeInfo<FBTDecorator_Loop>("Loop", "반복", EBTNodeCategory::Decorator,
		{ IntParam("NumLoops", "횟수 (0 = 무한)", 3) }));
	Registry.Register(MakeInfo<FBTDecorator_TimeLimit>("TimeLimit", "시간 제한", EBTNodeCategory::Decorator,
		{ FloatParam("TimeLimit", "제한(초)", 5.0f) }));
	Registry.Register(MakeInfo<FBTDecorator_Inverter>("Inverter", "결과 반전", EBTNodeCategory::Decorator));
	Registry.Register(MakeInfo<FBTDecorator_ForceSuccess>("ForceSuccess", "항상 성공", EBTNodeCategory::Decorator));

	Registry.Register(MakeInfo<FBTTask_Wait>("Wait", "대기", EBTNodeCategory::Task,
		{ FloatParam("WaitTime", "대기(초)", 1.0f), FloatParam("RandomDeviation", "무작위 편차(초)", 0.0f) }));
	Registry.Register(MakeInfo<FBTTask_SetBlackboard>("SetBlackboard", "블랙보드 설정", EBTNodeCategory::Task,
		{ StringParam("Key", "키", ""), StringParam("Value", "값", "") }));
	Registry.Register(MakeInfo<FBTTask_Log>("Log", "로그", EBTNodeCategory::Task,
		{ StringParam("Message", "메시지", "") }));
}

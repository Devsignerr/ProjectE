#include "AI/LuaNodes.h"

#include "AI/AIModule.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"

#include <sstream>

namespace
{
	// 노드 하나가 가진 스크립트 객체 (처음 쓸 때 만들고 노드와 함께 파기)
	class FLuaNodeObject
	{
	public:
		~FLuaNodeObject()
		{
			if (Hooks != nullptr && Handle != 0 && Hooks->DestroyObject)
			{
				Hooks->DestroyObject(Handle);
			}
		}

		void Initialize(const FBTNodeParams& Params)
		{
			Script     = Params.GetString("Script");
			Properties = Params.GetString("Properties");
		}

		// 만들 수 없으면(스크립트 없음, 플레이 중 아님, 로드 오류) false. 실패는 한 번만 시도한다
		bool Ensure(FBehaviorTreeInstance& Tree)
		{
			if (Handle != 0)
			{
				return true;
			}
			if (bTried)
			{
				return false;
			}
			bTried                              = true;
			const FBehaviorTreeContext& Context = Tree.GetContext();
			if (Context.AI == nullptr || Script.empty() || !Context.AI->GetScriptHooks().CreateObject)
			{
				E_LOG(LogAI, Warning, "Lua 노드: 스크립트가 없거나 플레이 중인 AI 시스템 밖에서 실행되었습니다 ({})", Script.empty() ? "<비어 있음>" : Script);
				return false;
			}
			Hooks  = &Context.AI->GetScriptHooks();
			Handle = Hooks->CreateObject(Script, Properties, Context.Self);
			return Handle != 0;
		}

		EAIScriptResult Call(FBehaviorTreeInstance& Tree, const char* Method, const float* DeltaSeconds = nullptr)
		{
			if (!Ensure(Tree) || !Hooks->Call)
			{
				return EAIScriptResult::Error;
			}
			return Hooks->Call(Handle, Method, DeltaSeconds);
		}

	private:
		std::string           Script;
		std::string           Properties;
		const FAIScriptHooks* Hooks  = nullptr; // FAISystem 소유 (노드보다 오래 산다)
		uint64                Handle = 0;
		bool                  bTried = false;
	};

	// 태스크 결과 해석. WhenNil: nil을 돌려줬을 때 상태
	EBTStatus ToTaskStatus(EAIScriptResult Result, EBTStatus WhenNil, EBTStatus WhenMissing)
	{
		switch (Result)
		{
		case EAIScriptResult::NotFound: return WhenMissing;
		case EAIScriptResult::Nil:      return WhenNil;
		case EAIScriptResult::Running:  return EBTStatus::Running;
		case EAIScriptResult::True:
		case EAIScriptResult::Success:  return EBTStatus::Success;
		case EAIScriptResult::Other:
			E_LOG(LogAI, Warning, "Lua 태스크: \"Running\"/\"Success\"/\"Failure\" 또는 true/false를 돌려주세요 (Success로 처리)");
			return EBTStatus::Success;
		default:                        return EBTStatus::Failure; // False/Failure/Error
		}
	}

	class FBTTask_Lua final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { Object.Initialize(Params); }

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			if (!Object.Ensure(Tree))
			{
				return EBTStatus::Failure;
			}
			return ToTaskStatus(Object.Call(Tree, "OnExecute"), EBTStatus::Success, EBTStatus::Success);
		}

		EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			return ToTaskStatus(Object.Call(Tree, "OnTick", &DeltaTime), EBTStatus::Running, EBTStatus::Success);
		}

		void OnAbort(FBehaviorTreeInstance& Tree) override { Object.Call(Tree, "OnAbort"); }

	private:
		FLuaNodeObject Object;
	};

	class FBTDecorator_Lua final : public FBTDecoratorNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			Object.Initialize(Params);
			AbortMode = BehaviorTreeTypes::ParseAbortMode(Params.GetString("AbortMode", "None")).value_or(EBTAbortMode::None);
			std::stringstream Stream(Params.GetString("ObservedKeys"));
			std::string       Key;
			while (std::getline(Stream, Key, ','))
			{
				const size_t First = Key.find_first_not_of(" \t");
				const size_t Last  = Key.find_last_not_of(" \t");
				if (First != std::string::npos)
				{
					ObservedKeys.push_back(Key.substr(First, Last - First + 1));
				}
			}
		}

		bool CalculateCondition(FBehaviorTreeInstance& Tree) override
		{
			if (!Object.Ensure(Tree))
			{
				return false;
			}
			const EAIScriptResult Result = Object.Call(Tree, "CanExecute");
			return Result == EAIScriptResult::NotFound || Result == EAIScriptResult::True || Result == EAIScriptResult::Success;
		}

		void         GetObservedKeys(std::vector<std::string>& OutKeys) const override { OutKeys.insert(OutKeys.end(), ObservedKeys.begin(), ObservedKeys.end()); }
		EBTAbortMode GetAbortMode() const override { return AbortMode; }

	private:
		FLuaNodeObject           Object;
		std::vector<std::string> ObservedKeys;
		EBTAbortMode             AbortMode = EBTAbortMode::None;
	};

	class FBTService_Lua final : public FBTServiceNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { Object.Initialize(Params); }

		void OnBecomeRelevant(FBehaviorTreeInstance& Tree) override { Object.Call(Tree, "OnBecomeRelevant"); }
		void OnCeaseRelevant(FBehaviorTreeInstance& Tree) override { Object.Call(Tree, "OnCeaseRelevant"); }
		void OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override { Object.Call(Tree, "OnTick", &DeltaTime); }

	private:
		FLuaNodeObject Object;
	};

	template <typename TNode>
	FBTNodeInfo MakeLuaInfo(const char* Name, const char* DisplayName, EBTNodeCategory Category, std::vector<FBTParamDesc> Extra = {})
	{
		FBTNodeInfo Info;
		Info.Name        = Name;
		Info.DisplayName = DisplayName;
		Info.Category    = Category;
		Info.Params      = {
			{ "Script", "스크립트 (.lua)", EPropertyType::String, FBTParamValue(std::string()), {} },
			{ "Properties", "프로퍼티 오버라이드 (JSON)", EPropertyType::String, FBTParamValue(std::string()), {} },
		};
		Info.Params.insert(Info.Params.end(), Extra.begin(), Extra.end());
		Info.Factory = []() -> std::unique_ptr<FBTNode> { return std::make_unique<TNode>(); };
		Info.Owner   = FBehaviorTreeNodeRegistry::EngineOwner;
		return Info;
	}
} // namespace

void RegisterLuaBehaviorTreeNodes(FBehaviorTreeNodeRegistry& Registry)
{
	Registry.Register(MakeLuaInfo<FBTTask_Lua>("LuaTask", "Lua 태스크", EBTNodeCategory::Task));
	Registry.Register(MakeLuaInfo<FBTDecorator_Lua>("LuaDecorator", "Lua 데코레이터", EBTNodeCategory::Decorator,
		{
			{ "ObservedKeys", "관찰 키 (쉼표 구분)", EPropertyType::String, FBTParamValue(std::string()), {} },
			{ "AbortMode", "중단 모드", EPropertyType::String, FBTParamValue(std::string("None")), { "None", "Self", "LowerPriority", "Both" } },
		}));
	Registry.Register(MakeLuaInfo<FBTService_Lua>("LuaService", "Lua 서비스", EBTNodeCategory::Service));
}

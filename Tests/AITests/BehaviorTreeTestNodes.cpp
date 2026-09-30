#include "BehaviorTreeTestNodes.h"

namespace
{
	class FTestTask final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			Name   = Params.GetString("Name");
			Result = Params.GetString("Result") == "Failure" ? EBTStatus::Failure : EBTStatus::Success;
			Ticks  = Params.GetInt("Ticks");
		}

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			BTTest::Log().push_back("exec:" + Name);
			Remaining = Ticks;
			return Ticks == 0 ? Result : EBTStatus::Running;
		}

		EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			(void)Tree;
			(void)DeltaTime;
			BTTest::Log().push_back("tick:" + Name);
			if (Remaining < 0)
			{
				return EBTStatus::Running;
			}
			--Remaining;
			return Remaining <= 0 ? Result : EBTStatus::Running;
		}

		void OnAbort(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			BTTest::Log().push_back("abort:" + Name);
		}

	private:
		std::string Name;
		EBTStatus   Result    = EBTStatus::Success;
		int32       Ticks     = 0;
		int32       Remaining = 0;
	};

	class FTestService final : public FBTServiceNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override { Name = Params.GetString("Name"); }
		void OnBecomeRelevant(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			BTTest::Log().push_back("svc_begin:" + Name);
		}
		void OnCeaseRelevant(FBehaviorTreeInstance& Tree) override
		{
			(void)Tree;
			BTTest::Log().push_back("svc_end:" + Name);
		}
		void OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			(void)Tree;
			(void)DeltaTime;
			BTTest::Log().push_back("svc_tick:" + Name);
		}

	private:
		std::string Name;
	};
} // namespace

namespace BTTest
{
	std::vector<std::string>& Log()
	{
		static std::vector<std::string> Entries;
		return Entries;
	}

	std::string JoinLog()
	{
		std::string Result;
		for (const std::string& Entry : Log())
		{
			if (!Result.empty())
			{
				Result += ',';
			}
			Result += Entry;
		}
		return Result;
	}

	FScope::FScope()
	{
		Log().clear();
		FBehaviorTreeNodeRegistry& Registry = FBehaviorTreeNodeRegistry::Get();

		FBTNodeInfo TaskInfo;
		TaskInfo.Name     = "TestTask";
		TaskInfo.Category = EBTNodeCategory::Task;
		TaskInfo.Params   = {
			{ "Name", "이름", EPropertyType::String, FBTParamValue(std::string()), {} },
			{ "Result", "결과", EPropertyType::String, FBTParamValue(std::string("Success")), { "Success", "Failure" } },
			{ "Ticks", "틱 수", EPropertyType::Int32, FBTParamValue(int32(0)), {} },
		};
		TaskInfo.Factory = []() -> std::unique_ptr<FBTNode> { return std::make_unique<FTestTask>(); };
		TaskInfo.Owner   = Owner;
		Registry.Register(std::move(TaskInfo));

		FBTNodeInfo ServiceInfo;
		ServiceInfo.Name     = "TestService";
		ServiceInfo.Category = EBTNodeCategory::Service;
		ServiceInfo.Params   = { { "Name", "이름", EPropertyType::String, FBTParamValue(std::string()), {} } };
		ServiceInfo.Factory  = []() -> std::unique_ptr<FBTNode> { return std::make_unique<FTestService>(); };
		ServiceInfo.Owner    = Owner;
		Registry.Register(std::move(ServiceInfo));
	}

	FScope::~FScope()
	{
		FBehaviorTreeNodeRegistry::Get().UnregisterOwner(Owner);
		Log().clear();
	}

	FBTNodeDesc Task(const std::string& Name, const std::string& Result, int32 Ticks)
	{
		FBTNodeDesc Node;
		Node.Type   = "TestTask";
		Node.Params = { { "Name", Name }, { "Result", Result }, { "Ticks", Ticks } };
		return Node;
	}

	FBTNodeDesc Service(const std::string& Name, float Interval)
	{
		FBTNodeDesc Node;
		Node.Type   = "TestService";
		Node.Params = { { "Name", Name }, { "Interval", Interval } };
		return Node;
	}

	FBTNodeDesc Composite(const std::string& Type, std::vector<FBTNodeDesc> Children)
	{
		FBTNodeDesc Node;
		Node.Type     = Type;
		Node.Children = std::move(Children);
		return Node;
	}

	FBTNodeDesc MakeNode(const std::string& Type, std::vector<FBTParam> Params)
	{
		FBTNodeDesc Node;
		Node.Type   = Type;
		Node.Params = std::move(Params);
		return Node;
	}

	FBTNodeDesc BlackboardCondition(const std::string& Key, const std::string& Operation, const std::string& Value, const std::string& AbortMode)
	{
		return MakeNode("Blackboard", { { "Key", Key }, { "Operation", Operation }, { "Value", Value }, { "AbortMode", AbortMode } });
	}

	FBTNodeDesc With(FBTNodeDesc Node, FBTNodeDesc DecoratorOrService)
	{
		const FBTNodeInfo* Info = FBehaviorTreeNodeRegistry::Get().Find(DecoratorOrService.Type);
		if (Info && Info->Category == EBTNodeCategory::Service)
		{
			Node.Services.push_back(std::move(DecoratorOrService));
		}
		else
		{
			Node.Decorators.push_back(std::move(DecoratorOrService));
		}
		return Node;
	}

	FBehaviorTreeAsset MakeAsset(FBTNodeDesc Root, std::vector<FBlackboardKeyDesc> Keys)
	{
		FBehaviorTreeAsset Asset;
		Asset.BlackboardKeys = std::move(Keys);
		Asset.Root           = std::move(Root);
		Asset.AssignMissingNodeIds();
		return Asset;
	}
} // namespace BTTest

#include "AI/AIModule.h"
#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/GameModuleHost.h"

namespace
{
	// 게임 모듈의 C++ 태스크: 블랙보드 "Hits"를 1 늘리고 성공
	class FGameTask_Hit final : public FBTTaskNode
	{
	public:
		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			FBlackboard& Blackboard = Tree.GetBlackboard();
			Blackboard.SetInt("Hits", Blackboard.Get<int32>("Hits").value_or(0) + 1);
			return EBTStatus::Success;
		}
	};

	// OnLoad에서 Owner를 비운 채 등록 → 호스트가 정한 등록 소유자(모듈 이름)가 된다
	class FTestGameModule final : public IGameModule
	{
	public:
		void OnLoad() override
		{
			FBTNodeInfo Info;
			Info.Name        = "GameHit";
			Info.DisplayName = "게임: 맞히기";
			Info.Category    = EBTNodeCategory::Task;
			Info.Factory     = []() -> std::unique_ptr<FBTNode> { return std::make_unique<FGameTask_Hit>(); };
			E_EXPECT_TRUE(FBehaviorTreeNodeRegistry::Get().Register(std::move(Info)));
		}
	};
} // namespace

E_TEST(GameModule_RegistersBehaviorTreeNodesAndUnloadRemovesThem)
{
	RegisterAITypes(); // 언로드 정리 콜백 등록

	FTestGameModule Module;
	FGameModuleHost Host;
	Host.Attach(Module, "TestGame");

	const FBTNodeInfo* Info = FBehaviorTreeNodeRegistry::Get().Find("GameHit");
	E_EXPECT_TRUE(Info != nullptr && Info->Owner == "TestGame");

	// 에셋에서 게임 노드를 쓸 수 있다
	FBehaviorTreeAsset Asset;
	Asset.BlackboardKeys = { { "Hits", EBlackboardKeyType::Int } };
	FBTNodeDesc Root;
	Root.Type = "GameHit";
	Asset.Root = Root;
	{
		FBehaviorTreeInstance Tree;
		E_EXPECT_TRUE(Tree.Initialize(Asset));
		Tree.Start();
		Tree.Tick(0.1f);
		E_EXPECT_TRUE(Tree.GetBlackboard().Get<int32>("Hits").value_or(0) >= 1);
	} // 언로드 전에 트리를 파기한다 (FBehaviorTreeNodeRegistry 주석)

	// 엔진 기본 노드는 게임 모듈 소유가 아니다
	E_EXPECT_TRUE(FBehaviorTreeNodeRegistry::Get().Find("MoveTo")->Owner == FBehaviorTreeNodeRegistry::EngineOwner);

	Host.Unload();
	E_EXPECT_TRUE(FBehaviorTreeNodeRegistry::Get().Find("GameHit") == nullptr);
	E_EXPECT_FALSE(Asset.Validate()); // 모르는 노드 타입
	E_EXPECT_TRUE(FBehaviorTreeNodeRegistry::Get().Find("Wait") != nullptr);
}

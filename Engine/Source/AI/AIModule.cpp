#include "AI/AIModule.h"

#include "AI/AIComponents.h"
#include "AI/AITasks.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"
#include "AI/LuaNodes.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/GameModuleHost.h"

E_DEFINE_LOG_CATEGORY(LogAI, Log)

void RegisterAITypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();

	Registry.RegisterType<FBehaviorTreeComponent>("BehaviorTreeComponent", "비헤이비어 트리")
		.Property(&FBehaviorTreeComponent::Asset, "Asset", "트리 에셋").AssetFilter(".ebt")
		.Property(&FBehaviorTreeComponent::bAutoStart, "AutoStart", "플레이 시 자동 시작")
		.AsComponent();

	Registry.RegisterType<FNavAgentComponent>("NavAgentComponent", "내비 에이전트")
		.Property(&FNavAgentComponent::MaxSpeed, "MaxSpeed", "최대 속도 (cm/s)").Range(0.0f, 10000.0f, 1.0f)
		.Property(&FNavAgentComponent::AcceptanceRadius, "AcceptanceRadius", "도착 반경 (cm)").Range(0.0f, 10000.0f, 1.0f)
		.Property(&FNavAgentComponent::TurnSpeedDegrees, "TurnSpeedDegrees", "회전 속도 (도/초)").Range(0.0f, 3600.0f, 1.0f)
		.Property(&FNavAgentComponent::bOrientToMovement, "OrientToMovement", "이동 방향으로 회전")
		.AsComponent();

	Registry.RegisterType<FNavMeshComponent>("NavMeshComponent", "내비메시")
		.Property(&FNavMeshComponent::NavMeshAsset, "NavMeshAsset", "구운 내비메시", PF_ReadOnly).AssetFilter(".enav")
		.Property(&FNavMeshComponent::AgentRadius, "AgentRadius", "에이전트 반경 (cm)").Range(1.0f, 1000.0f, 1.0f)
		.Property(&FNavMeshComponent::AgentHeight, "AgentHeight", "에이전트 키 (cm)").Range(1.0f, 1000.0f, 1.0f)
		.Property(&FNavMeshComponent::AgentMaxClimb, "AgentMaxClimb", "오를 수 있는 턱 (cm)").Range(0.0f, 500.0f, 1.0f)
		.Property(&FNavMeshComponent::AgentMaxSlopeDegrees, "AgentMaxSlopeDegrees", "최대 경사 (도)").Range(0.0f, 89.0f, 1.0f)
		.Property(&FNavMeshComponent::CellSize, "CellSize", "셀 크기 (cm)").Range(1.0f, 200.0f, 1.0f)
		.Property(&FNavMeshComponent::CellHeight, "CellHeight", "셀 높이 (cm)").Range(1.0f, 200.0f, 1.0f)
		.AsComponent();

	RegisterAITasks(FBehaviorTreeNodeRegistry::Get());
	RegisterLuaBehaviorTreeNodes(FBehaviorTreeNodeRegistry::Get());

	// 게임 모듈이 OnLoad에서 등록한 C++ 노드는 언로드 때 함께 해제한다 (게임 모듈은 플레이 중에 언로드되지 않는다 — 트리는 EndPlay에서 이미 파기됨)
	FGameModuleHost::AddUnloadCleanup([](const std::string& Owner) {
		const int32 Removed = FBehaviorTreeNodeRegistry::Get().UnregisterOwner(Owner);
		if (Removed > 0)
		{
			E_LOG(LogAI, Display, "게임 모듈 {}의 비헤이비어 트리 노드 {}개 해제", Owner, Removed);
		}
	});
}

#include "AI/AITasks.h"

#include "AI/AIComponents.h"
#include "AI/AIModule.h"
#include "AI/AISystem.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "AI/BehaviorTree/BehaviorTreeNodeRegistry.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Scene.h"

namespace
{
	// 블랙보드 키(Vector 또는 Entity)가 가리키는 월드 위치. 키가 없거나 설정 안 됐거나 엔티티가 없으면 false
	bool ResolveTarget(FBehaviorTreeInstance& Tree, const std::string& Key, FVector3& OutPoint)
	{
		const FBlackboard& Blackboard = Tree.GetBlackboard();
		if (const std::optional<FVector3> Point = Blackboard.Get<FVector3>(Key))
		{
			OutPoint = *Point;
			return true;
		}
		const std::optional<FEntity> Target = Blackboard.Get<FEntity>(Key);
		FScene*                      Scene  = Tree.GetContext().Scene;
		if (Target && Scene != nullptr && Scene->GetRegistry().IsValid(*Target))
		{
			OutPoint = Scene->GetTransform(*Target).GetWorldPosition();
			return true;
		}
		return false;
	}

	bool HasSceneContext(const FBehaviorTreeInstance& Tree, const char* TaskName)
	{
		const FBehaviorTreeContext& Context = Tree.GetContext();
		if (Context.Scene != nullptr && Context.AI != nullptr && Context.Scene->GetRegistry().IsValid(Context.Self))
		{
			return true;
		}
		E_LOG(LogAI, Warning, "{}: 씬/AI 시스템 없이 실행된 트리입니다", TaskName);
		return false;
	}

	EBTStatus ToTaskStatus(EAIMoveStatus Status)
	{
		switch (Status)
		{
		case EAIMoveStatus::Moving:    return EBTStatus::Running;
		case EAIMoveStatus::Succeeded: return EBTStatus::Success;
		default:                       return EBTStatus::Failure;
		}
	}

	// 블랙보드 목표(Vector/Entity)까지 내비메시 경로로 이동. 목표가 RepathDistance 넘게 움직이면 경로를 다시 찾는다
	class FBTTask_MoveTo final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			TargetKey        = Params.GetString("TargetKey");
			AcceptanceRadius = Params.GetFloat("AcceptanceRadius", -1.0f);
			RepathDistance   = Params.GetFloat("RepathDistance", 50.0f);
		}

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			if (!HasSceneContext(Tree, "MoveTo") || !ResolveTarget(Tree, TargetKey, RequestedGoal))
			{
				return EBTStatus::Failure;
			}
			const FBehaviorTreeContext& Context = Tree.GetContext();
			return ToTaskStatus(Context.AI->RequestMove(Context.Self, RequestedGoal, AcceptanceRadius));
		}

		EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			(void)DeltaTime;
			const FBehaviorTreeContext& Context = Tree.GetContext();
			FVector3                    Goal;
			if (!ResolveTarget(Tree, TargetKey, Goal))
			{
				Context.AI->StopMove(Context.Self);
				return EBTStatus::Failure; // 목표가 사라졌다 (키 비움, 엔티티 파괴)
			}
			if (FVector3::DistanceSquared(Goal, RequestedGoal) > RepathDistance * RepathDistance)
			{
				RequestedGoal = Goal;
				return ToTaskStatus(Context.AI->RequestMove(Context.Self, Goal, AcceptanceRadius));
			}
			return ToTaskStatus(Context.AI->GetMoveStatus(Context.Self));
		}

		void OnAbort(FBehaviorTreeInstance& Tree) override
		{
			const FBehaviorTreeContext& Context = Tree.GetContext();
			if (Context.AI != nullptr)
			{
				Context.AI->StopMove(Context.Self);
			}
		}

	private:
		std::string TargetKey;
		float       AcceptanceRadius = -1.0f;
		float       RepathDistance   = 50.0f;
		FVector3    RequestedGoal;
	};

	// 블랙보드 목표 쪽으로 제자리 회전 (FNavAgentComponent.TurnSpeedDegrees)
	class FBTTask_RotateTo final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			TargetKey = Params.GetString("TargetKey");
			Tolerance = Params.GetFloat("ToleranceDegrees", 5.0f);
		}

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			if (!HasSceneContext(Tree, "RotateTo"))
			{
				return EBTStatus::Failure;
			}
			return Turn(Tree, 0.0f);
		}

		EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override { return Turn(Tree, DeltaTime); }

	private:
		EBTStatus Turn(FBehaviorTreeInstance& Tree, float DeltaTime)
		{
			FVector3 Point;
			if (!ResolveTarget(Tree, TargetKey, Point))
			{
				return EBTStatus::Failure;
			}
			const FBehaviorTreeContext& Context  = Tree.GetContext();
			const FNavAgentComponent*   Agent    = Context.Scene->GetRegistry().TryGet<FNavAgentComponent>(Context.Self);
			const float                 Speed    = Agent ? Agent->TurnSpeedDegrees : FNavAgentComponent{}.TurnSpeedDegrees;
			const bool                  bFacing  = FAISystem::TurnTowards(*Context.Scene, Context.Self, Point, Speed * DeltaTime, Tolerance);
			return bFacing ? EBTStatus::Success : EBTStatus::Running;
		}

		std::string TargetKey;
		float       Tolerance = 5.0f;
	};

	// 애니메이션 클립 재생 (자기 엔티티의 FAnimationComponent). bWaitForFinish면 클립 길이만큼 기다린다
	class FBTTask_PlayAnimation final : public FBTTaskNode
	{
	public:
		void Initialize(const FBTNodeParams& Params) override
		{
			ClipName        = Params.GetString("ClipName");
			BlendTime       = Params.GetFloat("BlendTime", -1.0f);
			bWaitForFinish  = Params.GetBool("WaitForFinish", true);
		}

		EBTStatus OnExecute(FBehaviorTreeInstance& Tree) override
		{
			if (!HasSceneContext(Tree, "PlayAnimation"))
			{
				return EBTStatus::Failure;
			}
			const FBehaviorTreeContext& Context = Tree.GetContext();
			if (!FAnimationSystem::Play(*Context.Scene, Context.Self, ClipName, BlendTime))
			{
				E_LOG(LogAI, Warning, "PlayAnimation: 클립 '{}'를 재생하지 못했습니다", ClipName);
				return EBTStatus::Failure;
			}
			if (!bWaitForFinish)
			{
				return EBTStatus::Success;
			}
			Remaining = FAnimationSystem::GetCurrentClipDuration(*Context.Scene, Context.Self);
			return Remaining > 0.0f ? EBTStatus::Running : EBTStatus::Success;
		}

		EBTStatus OnTick(FBehaviorTreeInstance& Tree, float DeltaTime) override
		{
			(void)Tree;
			Remaining -= DeltaTime;
			return Remaining <= 0.0f ? EBTStatus::Success : EBTStatus::Running;
		}

	private:
		std::string ClipName;
		float       BlendTime      = -1.0f;
		bool        bWaitForFinish = true;
		float       Remaining      = 0.0f;
	};

	template <typename TNode>
	FBTNodeInfo MakeTaskInfo(const char* Name, const char* DisplayName, std::vector<FBTParamDesc> Params)
	{
		FBTNodeInfo Info;
		Info.Name        = Name;
		Info.DisplayName = DisplayName;
		Info.Category    = EBTNodeCategory::Task;
		Info.Params      = std::move(Params);
		Info.Factory     = []() -> std::unique_ptr<FBTNode> { return std::make_unique<TNode>(); };
		Info.Owner       = FBehaviorTreeNodeRegistry::EngineOwner;
		return Info;
	}
} // namespace

void RegisterAITasks(FBehaviorTreeNodeRegistry& Registry)
{
	Registry.Register(MakeTaskInfo<FBTTask_MoveTo>("MoveTo", "이동 (MoveTo)",
		{
			{ "TargetKey", "목표 키 (Vector/Entity)", EPropertyType::String, FBTParamValue(std::string()), {} },
			{ "AcceptanceRadius", "도착 반경 (cm, -1 = 에이전트 값)", EPropertyType::Float, FBTParamValue(-1.0f), {} },
			{ "RepathDistance", "목표 이동 시 재탐색 거리 (cm)", EPropertyType::Float, FBTParamValue(50.0f), {} },
		}));
	Registry.Register(MakeTaskInfo<FBTTask_RotateTo>("RotateTo", "회전 (RotateTo)",
		{
			{ "TargetKey", "목표 키 (Vector/Entity)", EPropertyType::String, FBTParamValue(std::string()), {} },
			{ "ToleranceDegrees", "허용 각도 (도)", EPropertyType::Float, FBTParamValue(5.0f), {} },
		}));
	Registry.Register(MakeTaskInfo<FBTTask_PlayAnimation>("PlayAnimation", "애니메이션 재생",
		{
			{ "ClipName", "클립 이름", EPropertyType::String, FBTParamValue(std::string()), {} },
			{ "BlendTime", "블렌드 (초, -1 = 기본)", EPropertyType::Float, FBTParamValue(-1.0f), {} },
			{ "WaitForFinish", "끝날 때까지 대기", EPropertyType::Bool, FBTParamValue(true), {} },
		}));
}

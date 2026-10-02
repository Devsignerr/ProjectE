#include "AI/AISystem.h"

#include "Core/Profiling.h"
#include "AI/AIComponents.h"
#include "AI/AIModule.h"
#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "AI/BehaviorTree/BehaviorTreeInstance.h"
#include "Core/StringConv.h"
#include "Scene/Scene.h"

namespace
{
	constexpr float WaypointReachDistance = 5.0f; // 경로점 도달 판정 (cm)

	float HorizontalDistance(const FVector3& A, const FVector3& B)
	{
		const float X = B.X - A.X;
		const float Y = B.Y - A.Y;
		return FMath::Sqrt(X * X + Y * Y);
	}

	// 월드 트랜스폼(위치/회전, 스케일 유지)을 로컬 값으로 되돌려 쓴다 (소켓 부착이면 소켓 기준)
	void SetWorldPose(FScene& Scene, FEntity Entity, const FVector3& Position, const FQuat& Rotation)
	{
		FTransformComponent& Transform = Scene.GetTransform(Entity);
		FVector3             OldPosition;
		FQuat                OldRotation;
		FVector3             Scale;
		Transform.WorldMatrix.Decompose(OldPosition, OldRotation, Scale);
		const FMatrix4x4 World = FMatrix4x4::MakeTransform(Position, Rotation, Scale);
		const FMatrix4x4 Local = World * Scene.GetParentWorldMatrix(Entity).GetInverse();
		Local.Decompose(Transform.Position, Transform.Rotation, Transform.Scale);
		// 같은 프레임의 다음 계산(경로 진행, 다른 태스크)이 새 위치를 보도록 월드 행렬도 맞춘다
		Transform.WorldMatrix = World;
	}
} // namespace

FAISystem::FAISystem() = default;

FAISystem::~FAISystem()
{
	// 트리 소멸자의 Stop → 태스크 OnAbort(MoveTo → StopMove)가 Agents를 다시 조회하므로 맵이 온전할 때 먼저 멈춘다
	End();
}

void FAISystem::Begin(FScene& InScene)
{
	if (IsActive())
	{
		End();
	}
	Scene            = &InScene;
	bWarnedNoNavMesh = false;

	// 내비메시: 첫 FNavMeshComponent의 구운 파일
	std::string NavMeshAsset;
	Scene->GetRegistry().View<FNavMeshComponent>().Each([&](FEntity, FNavMeshComponent& Component) {
		if (NavMeshAsset.empty() && !Component.NavMeshAsset.empty())
		{
			NavMeshAsset = Component.NavMeshAsset;
		}
	});
	if (!NavMeshAsset.empty())
	{
		FNavMesh    Loaded;
		std::string Error;
		if (Loaded.LoadFromFile(ContentDirectory / FStringConv::ToWide(NavMeshAsset), &Error))
		{
			NavMesh = std::move(Loaded);
			E_LOG(LogAI, Log, "내비메시 로드: {} (폴리곤 {}개)", NavMeshAsset, NavMesh.GetPolygonCount());
		}
		else
		{
			E_LOG(LogAI, Warning, "내비메시를 읽지 못했습니다: {} — {}", NavMeshAsset, Error);
		}
	}

	SyncComponents();
}

void FAISystem::End()
{
	// 트리 Stop(태스크 OnAbort)은 맵을 조회만 하고 항목을 추가/삭제하지 않는다
	for (auto& [Id, Agent] : Agents)
	{
		if (Agent.Tree)
		{
			Agent.Tree->Stop();
		}
		if (Scene != nullptr && Agent.Move.Status == EAIMoveStatus::Moving)
		{
			FinishMove(FEntity::FromId(Id), Agent.Move, EAIMoveStatus::Idle);
		}
	}
	for (auto& [Id, Agent] : Agents)
	{
		Agent.Tree.reset(); // 이미 멈췄으므로 소멸자의 Stop은 아무것도 하지 않는다
	}
	Agents.clear();
	NavMesh.Reset();
	Scene = nullptr;
}

void FAISystem::Update(FScene& InScene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("AI");
	if (Scene != &InScene)
	{
		return;
	}
	SyncComponents();

	// 트리 틱. 태스크가 이동을 요청해도 맵에 새 항목은 생기지 않지만(자기 엔티티는 이미 있다) 순회는 ID 스냅샷으로 한다
	std::vector<uint64> Ids;
	Ids.reserve(Agents.size());
	for (const auto& [Id, Agent] : Agents)
	{
		Ids.push_back(Id);
	}
	for (uint64 Id : Ids)
	{
		auto Found = Agents.find(Id);
		if (Found != Agents.end() && Found->second.Tree && Found->second.Tree->IsRunning())
		{
			Found->second.Tree->Tick(DeltaSeconds);
		}
	}

	for (auto& [Id, Agent] : Agents)
	{
		if (Agent.Move.Status == EAIMoveStatus::Moving)
		{
			UpdateMovement(FEntity::FromId(Id), Agent.Move, DeltaSeconds);
		}
	}
}

void FAISystem::SyncComponents()
{
	FRegistry& Registry = Scene->GetRegistry();

	// 파괴된 엔티티 / 컴포넌트가 없어진 트리 정리 (이동만 요청된 엔티티는 유지)
	for (auto It = Agents.begin(); It != Agents.end();)
	{
		const FEntity Entity = FEntity::FromId(It->first);
		if (!Registry.IsValid(Entity))
		{
			if (It->second.Tree)
			{
				It->second.Tree->Stop();
			}
			It = Agents.erase(It);
			continue;
		}
		if (It->second.Tree && !Registry.Has<FBehaviorTreeComponent>(Entity))
		{
			It->second.Tree->Stop();
			It->second.Tree.reset();
			It->second.TreeAsset.clear();
		}
		++It;
	}

	// 새 컴포넌트 / 에셋 경로가 바뀐 컴포넌트 (순회 중 맵만 바꾸고 레지스트리는 건드리지 않는다)
	std::vector<std::pair<FEntity, std::string>> ToCreate;
	Registry.View<FBehaviorTreeComponent>().Each([&](FEntity Entity, FBehaviorTreeComponent& Component) {
		const auto Found = Agents.find(Entity.ToId());
		if (Found == Agents.end())
		{
			if (Component.bAutoStart && !Component.Asset.empty())
			{
				ToCreate.emplace_back(Entity, Component.Asset);
			}
			return;
		}
		const FAgent& Agent = Found->second;
		const bool    bHasTreeState = Agent.Tree != nullptr || Agent.bTreeFailed;
		if (bHasTreeState && Agent.TreeAsset != Component.Asset && !Component.Asset.empty())
		{
			ToCreate.emplace_back(Entity, Component.Asset);
		}
		else if (!bHasTreeState && Component.bAutoStart && !Component.Asset.empty())
		{
			ToCreate.emplace_back(Entity, Component.Asset);
		}
	});
	for (const auto& [Entity, AssetPath] : ToCreate)
	{
		CreateTree(Entity, Agents[Entity.ToId()], AssetPath);
	}
}

const FBehaviorTreeAsset* FAISystem::LoadAsset(const std::string& AssetPath)
{
	if (const auto Found = AssetCache.find(AssetPath); Found != AssetCache.end())
	{
		return Found->second.get();
	}
	auto        Asset = std::make_unique<FBehaviorTreeAsset>();
	std::string Error;
	if (!Asset->LoadFromFile(ContentDirectory / FStringConv::ToWide(AssetPath), &Error))
	{
		E_LOG(LogAI, Error, "비헤이비어 트리를 읽지 못했습니다: {} — {}", AssetPath, Error);
		Asset.reset();
	}
	return (AssetCache[AssetPath] = std::move(Asset)).get();
}

bool FAISystem::CreateTree(FEntity Entity, FAgent& Agent, const std::string& AssetPath)
{
	if (Agent.Tree)
	{
		Agent.Tree->Stop();
		Agent.Tree.reset();
	}
	Agent.TreeAsset   = AssetPath;
	Agent.bTreeFailed = true;

	const FBehaviorTreeAsset* Asset = LoadAsset(AssetPath);
	if (Asset == nullptr)
	{
		return false;
	}
	FBehaviorTreeContext TreeContext;
	TreeContext.Scene = Scene;
	TreeContext.AI    = this;
	TreeContext.Self  = Entity;

	auto        Tree = std::make_unique<FBehaviorTreeInstance>();
	std::string Error;
	if (!Tree->Initialize(*Asset, TreeContext, &Error))
	{
		E_LOG(LogAI, Error, "비헤이비어 트리를 시작하지 못했습니다: {} — {}", AssetPath, Error);
		return false;
	}
	// 시작 중 노드(서비스 OnBecomeRelevant, Lua 노드)가 FindTree로 블랙보드를 찾으므로 먼저 등록하고 시작한다
	Agent.Tree        = std::move(Tree);
	Agent.bTreeFailed = false;
	Agent.Tree->Start();
	return true;
}

FBehaviorTreeInstance* FAISystem::FindTree(FEntity Entity)
{
	const auto Found = Agents.find(Entity.ToId());
	return Found != Agents.end() ? Found->second.Tree.get() : nullptr;
}

bool FAISystem::StartTree(FEntity Entity)
{
	if (!IsActive())
	{
		return false;
	}
	const FBehaviorTreeComponent* Component = Scene->GetRegistry().TryGet<FBehaviorTreeComponent>(Entity);
	if (Component == nullptr || Component->Asset.empty())
	{
		return false;
	}
	return CreateTree(Entity, Agents[Entity.ToId()], Component->Asset);
}

void FAISystem::StopTree(FEntity Entity)
{
	if (FBehaviorTreeInstance* Tree = FindTree(Entity))
	{
		Tree->Stop();
	}
}

void FAISystem::ReloadBehaviorTree(const std::string& AssetPath)
{
	AssetCache.erase(AssetPath);
	if (!IsActive())
	{
		return;
	}
	for (auto& [Id, Agent] : Agents)
	{
		if (Agent.TreeAsset == AssetPath)
		{
			CreateTree(FEntity::FromId(Id), Agent, AssetPath);
		}
	}
}

bool FAISystem::FindPath(const FVector3& Start, const FVector3& End, std::vector<FVector3>& OutPoints) const
{
	OutPoints.clear();
	if (!NavMesh.IsValid())
	{
		OutPoints = { Start, End };
		return true;
	}
	return NavMesh.FindPath(Start, End, OutPoints) != ENavPathResult::Failed;
}

EAIMoveStatus FAISystem::RequestMove(FEntity Entity, const FVector3& Goal, float AcceptanceRadius)
{
	if (!IsActive() || !Scene->GetRegistry().IsValid(Entity))
	{
		return EAIMoveStatus::Failed;
	}
	FMoveState& Move = Agents[Entity.ToId()].Move;
	if (AcceptanceRadius < 0.0f)
	{
		const FNavAgentComponent* Agent = Scene->GetRegistry().TryGet<FNavAgentComponent>(Entity);
		AcceptanceRadius                = Agent ? Agent->AcceptanceRadius : FNavAgentComponent{}.AcceptanceRadius;
	}
	Move.Goal             = Goal;
	Move.AcceptanceRadius = AcceptanceRadius;
	Move.Path.clear();
	Move.NextPoint = 1;

	const FVector3 Position = Scene->GetTransform(Entity).GetWorldPosition();
	if (HorizontalDistance(Position, Goal) <= AcceptanceRadius)
	{
		FinishMove(Entity, Move, EAIMoveStatus::Succeeded);
		return Move.Status;
	}
	if (!NavMesh.IsValid() && !bWarnedNoNavMesh)
	{
		E_LOG(LogAI, Warning, "내비메시가 없어 MoveTo가 목표까지 직선으로 이동합니다 (FNavMeshComponent + 굽기 필요)");
		bWarnedNoNavMesh = true;
	}
	if (!FindPath(Position, Goal, Move.Path) || Move.Path.size() < 2)
	{
		FinishMove(Entity, Move, EAIMoveStatus::Failed);
		return Move.Status;
	}
	Move.Status = EAIMoveStatus::Moving;
	return Move.Status;
}

EAIMoveStatus FAISystem::GetMoveStatus(FEntity Entity) const
{
	const auto Found = Agents.find(Entity.ToId());
	return Found != Agents.end() ? Found->second.Move.Status : EAIMoveStatus::Idle;
}

void FAISystem::StopMove(FEntity Entity)
{
	const auto Found = Agents.find(Entity.ToId());
	if (Found != Agents.end() && IsActive())
	{
		FinishMove(Entity, Found->second.Move, EAIMoveStatus::Idle);
	}
}

const std::vector<FVector3>* FAISystem::GetMovePath(FEntity Entity) const
{
	const auto Found = Agents.find(Entity.ToId());
	if (Found == Agents.end() || Found->second.Move.Status != EAIMoveStatus::Moving)
	{
		return nullptr;
	}
	return &Found->second.Move.Path;
}

void FAISystem::FinishMove(FEntity Entity, FMoveState& Move, EAIMoveStatus Status)
{
	Move.Status = Status;
	Move.Path.clear();
	if (Move.bPhysicsDriven && MovementHooks.ApplyVelocity && Scene->GetRegistry().IsValid(Entity))
	{
		MovementHooks.ApplyVelocity(Entity, FVector3::ZeroVector); // 미끄러지지 않게 수평 속도를 멈춘다
	}
	Move.bPhysicsDriven = false;
}

void FAISystem::UpdateMovement(FEntity Entity, FMoveState& Move, float DeltaSeconds)
{
	FRegistry& Registry = Scene->GetRegistry();
	if (!Registry.IsValid(Entity))
	{
		Move.Status = EAIMoveStatus::Failed;
		return;
	}
	const FNavAgentComponent  Defaults;
	const FNavAgentComponent* AgentSettings = Registry.TryGet<FNavAgentComponent>(Entity);
	const FNavAgentComponent& Settings      = AgentSettings ? *AgentSettings : Defaults;

	const FVector3 Position = Scene->GetTransform(Entity).GetWorldPosition();
	if (HorizontalDistance(Position, Move.Goal) <= Move.AcceptanceRadius)
	{
		FinishMove(Entity, Move, EAIMoveStatus::Succeeded);
		return;
	}

	// 가까운 경로점은 건너뛴다. 끝까지 왔는데 목표 밖이면 부분 경로 끝 → 실패
	const float StepDistance = Settings.MaxSpeed * DeltaSeconds;
	while (Move.NextPoint < Move.Path.size() && HorizontalDistance(Position, Move.Path[Move.NextPoint]) <= WaypointReachDistance)
	{
		++Move.NextPoint;
	}
	if (Move.NextPoint >= Move.Path.size())
	{
		FinishMove(Entity, Move, EAIMoveStatus::Failed);
		return;
	}

	const FVector3 Waypoint = Move.Path[Move.NextPoint];
	FVector3       Flat(Waypoint.X - Position.X, Waypoint.Y - Position.Y, 0.0f);
	const float    FlatDistance = Flat.Length();
	Flat /= FMath::Max(FlatDistance, FMath::SmallNumber);

	if (MovementHooks.ApplyVelocity && MovementHooks.ApplyVelocity(Entity, Flat * Settings.MaxSpeed))
	{
		Move.bPhysicsDriven = true; // 물리가 옮긴다 (회전도 물리 몫)
		return;
	}
	Move.bPhysicsDriven = false;

	// 트랜스폼 직접 이동: 경로점 쪽으로 최대 StepDistance, 높이는 경로(메시 표면)를 따라 보간
	const float    Alpha       = FlatDistance > FMath::SmallNumber ? FMath::Min(1.0f, StepDistance / FlatDistance) : 1.0f;
	const FVector3 NewPosition = FVector3::Lerp(Position, Waypoint, Alpha);
	MoveEntityTo(Entity, NewPosition);
	if (Settings.bOrientToMovement)
	{
		TurnTowards(*Scene, Entity, Waypoint, Settings.TurnSpeedDegrees * DeltaSeconds, 0.0f);
	}
}

void FAISystem::MoveEntityTo(FEntity Entity, const FVector3& WorldPosition)
{
	FVector3 Position;
	FQuat    Rotation;
	FVector3 Scale;
	Scene->GetTransform(Entity).WorldMatrix.Decompose(Position, Rotation, Scale);
	SetWorldPose(*Scene, Entity, WorldPosition, Rotation);
}

bool FAISystem::TurnTowards(FScene& TargetScene, FEntity Entity, const FVector3& Point, float MaxDegrees, float ToleranceDegrees)
{
	FVector3 Position;
	FQuat    Rotation;
	FVector3 Scale;
	const FMatrix4x4& World = TargetScene.GetTransform(Entity).WorldMatrix;
	World.Decompose(Position, Rotation, Scale);

	const float DirectionX = Point.X - Position.X;
	const float DirectionY = Point.Y - Position.Y;
	if (DirectionX * DirectionX + DirectionY * DirectionY < FMath::SmallNumber)
	{
		return true; // 제자리
	}
	// 행벡터 규약: 0행 = 월드 앞(+X) 방향. Yaw = atan2(Y, X) (+Yaw = 오른쪽 = +Y)
	const float CurrentYaw = FMath::RadiansToDegrees(FMath::Atan2(World.M[0][1], World.M[0][0]));
	const float TargetYaw  = FMath::RadiansToDegrees(FMath::Atan2(DirectionY, DirectionX));
	float       Delta      = TargetYaw - CurrentYaw;
	while (Delta > 180.0f)
	{
		Delta -= 360.0f;
	}
	while (Delta < -180.0f)
	{
		Delta += 360.0f;
	}
	if (FMath::Abs(Delta) <= ToleranceDegrees)
	{
		return true;
	}
	const float Step = FMath::Clamp(Delta, -MaxDegrees, MaxDegrees);
	// 월드 Z 기준 회전을 앞에 곱한다 (A * B는 B 먼저 적용)
	SetWorldPose(TargetScene, Entity, Position, FQuat::FromEuler(0.0f, Step, 0.0f) * Rotation);
	return FMath::Abs(Delta - Step) <= ToleranceDegrees;
}

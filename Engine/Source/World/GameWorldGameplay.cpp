// FGameWorld의 게임플레이 규칙 부분 (Scene/Gameplay.h 체력·게임 모드). 게임플레이 틱에서 AI 뒤, 물리 앞.
//
// 서버(Standalone 포함):
//   1) 데미지 이벤트: FHealthComponent 런타임 큐(Gameplay::ApplyDamage가 쌓음)를 엔티티 인덱스 순으로 꺼내
//      대상 스크립트 OnDamaged(amount, instigator) → 게임 모듈 OnDamaged → 죽었으면 사망 처리.
//      이벤트 처리 중 생긴 데미지(반격 등)도 같은 틱에 몇 차례까지 이어서 처리한다
//   2) 사망 처리: 점수(게임 모드 진행 중, ScoreValue를 가해자 소유 플레이어에게 — Standalone은 소유자 없는 가해자 = 로컬 플레이어 0, 자기 자신 제외)
//      → 대상 스크립트 OnDeath(instigator) → 게임 모듈 OnDeath → 모든 스크립트 OnEntityDied(victim, instigator)
//      → DeathAction: Destroy = 스크립트 지연 파괴(OnDestroy 뒤), 리스폰 = 게임 모드 RespawnDelay 뒤 (게임 모드가 없으면 기본값 3초, < 0 = 안 함)
//   3) 리스폰: 체력 = MaxHealth, PlayerStart 리스폰은 FNetPlayerSpawner::FindPlayerStarts를 돌아가며 (없으면 제자리),
//      캐릭터 이동/강체 속도는 0 → 스크립트 OnRespawned() → 게임 모듈 OnRespawned. 폰을 다시 만들지 않으므로 NetId·소유권·프리팹이 그대로다
//   4) 매치: Gameplay::TickMatch (대기 → 진행 → 끝)
// 모든 역할: 게임 모드 MatchState가 바뀌면 모든 스크립트 OnMatchStateChanged(state 이름) (클라이언트는 복제된 값으로)
#include "World/GameWorld.h"

#include "Core/Log.h"
#include "Network/NetPlayerSpawner.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <format>
#include <utility>

E_DECLARE_LOG_CATEGORY(LogGameplay)
E_DEFINE_LOG_CATEGORY(LogGameplay, Log)

namespace
{
	constexpr int32 MaxDamagePasses = 4; // 이벤트 처리 중 생긴 데미지를 같은 틱에 이어서 처리하는 횟수

	FGameRpcValue EntityOrNil(const FScene& Scene, FEntity Entity)
	{
		return Scene.GetRegistry().IsValid(Entity) ? FGameRpcValue::MakeEntity(Entity) : FGameRpcValue();
	}

	std::string DescribeEntity(FScene& Scene, FEntity Entity)
	{
		if (!Scene.GetRegistry().IsValid(Entity))
		{
			return "없음";
		}
		const FNameComponent* Name = Scene.GetRegistry().TryGet<FNameComponent>(Entity);
		return Name != nullptr ? Name->Name : std::string("이름 없음");
	}
} // namespace

void FGameWorld::TickGameplayRules(float DeltaSeconds)
{
	FRegistry& Registry = Scene->GetRegistry();
	if (Mode != ENetMode::Client)
	{
		DispatchDamageEvents();

		std::vector<FEntity> Due;
		Registry.View<FHealthComponent>().Each([&](FEntity Entity, FHealthComponent& Health) {
			if (Health.Runtime.RespawnTimer >= 0.0f)
			{
				Health.Runtime.RespawnTimer -= DeltaSeconds;
				if (Health.Runtime.RespawnTimer <= 0.0f)
				{
					Due.push_back(Entity);
				}
			}
		});
		std::sort(Due.begin(), Due.end(), [](FEntity A, FEntity B) { return A.Index < B.Index; });
		for (FEntity Entity : Due)
		{
			Respawn(Entity);
		}

		if (const FEntity GameModeEntity = Gameplay::FindGameMode(*Scene); GameModeEntity.IsValid())
		{
			Gameplay::TickMatch(Registry.Get<FGameModeComponent>(GameModeEntity), DeltaSeconds);
		}
	}

	// 매치 상태 변화 알림 (서버는 방금 바꾼 값, 클라이언트는 복제된 값)
	const FEntity GameModeEntity = Gameplay::FindGameMode(*Scene);
	const int32   State          = GameModeEntity.IsValid() ? static_cast<int32>(Registry.Get<FGameModeComponent>(GameModeEntity).MatchState) : -1;
	if (State != LastMatchState)
	{
		LastMatchState = State;
		if (State >= 0)
		{
			const FGameModeComponent& GameMode = Registry.Get<FGameModeComponent>(GameModeEntity);
			const char*               Name     = Gameplay::ToString(GameMode.MatchState);
			E_LOG(LogGameplay, Display, "매치 상태: {}{}", Name,
			      GameMode.MatchState == EMatchState::Ended ? std::format(" (승자 {}, 점수 {})", GameMode.WinnerPlayerId, GameMode.Scores) : std::string());
			Systems.Scripts->BroadcastMethod("OnMatchStateChanged", { FGameRpcValue::MakeString(Name) });
		}
	}
}

void FGameWorld::DispatchDamageEvents()
{
	FRegistry& Registry = Scene->GetRegistry();
	for (int32 Pass = 0; Pass < MaxDamagePasses; ++Pass)
	{
		std::vector<std::pair<FEntity, std::vector<FDamageEvent>>> Batch;
		Registry.View<FHealthComponent>().Each([&](FEntity Entity, FHealthComponent& Health) {
			if (!Health.Runtime.PendingEvents.empty())
			{
				Batch.emplace_back(Entity, std::move(Health.Runtime.PendingEvents));
				Health.Runtime.PendingEvents.clear();
			}
		});
		if (Batch.empty())
		{
			return;
		}
		std::sort(Batch.begin(), Batch.end(), [](const auto& A, const auto& B) { return A.first.Index < B.first.Index; });
		for (const auto& [Target, Events] : Batch)
		{
			for (const FDamageEvent& Event : Events)
			{
				if (!Registry.IsValid(Target))
				{
					break; // 앞 이벤트 처리 중 파괴됨
				}
				Systems.Scripts->InvokeMethod(Target, "OnDamaged", { FGameRpcValue::MakeNumber(Event.Amount), EntityOrNil(*Scene, Event.Instigator) });
				if (Systems.GameModule != nullptr)
				{
					Systems.GameModule->Damaged(*Scene, Target, Event.Amount, Event.Instigator);
				}
				if (Event.bKilled)
				{
					HandleDeath(Target, Event.Instigator);
				}
			}
		}
	}
	// 남은 이벤트(연쇄가 MaxDamagePasses를 넘음)는 다음 틱에 처리한다
}

void FGameWorld::HandleDeath(FEntity Victim, FEntity Instigator)
{
	FRegistry& Registry = Scene->GetRegistry();
	E_LOG(LogGameplay, Display, "사망: {} (가해자 {})", DescribeEntity(*Scene, Victim), DescribeEntity(*Scene, Instigator));

	// 점수 먼저 (사망 이벤트를 받는 쪽이 갱신된 점수를 본다)
	const FEntity GameModeEntity = Gameplay::FindGameMode(*Scene);
	if (const FHealthComponent* Health = Registry.TryGet<FHealthComponent>(Victim); Health != nullptr && Health->ScoreValue != 0 && GameModeEntity.IsValid())
	{
		FGameModeComponent& GameMode = Registry.Get<FGameModeComponent>(GameModeEntity);
		const int32         Scorer   = GetScoringPlayer(Instigator, Victim);
		if (GameMode.MatchState == EMatchState::InProgress && Scorer >= 0)
		{
			Gameplay::AddScore(GameMode, Scorer, Health->ScoreValue);
		}
	}

	Systems.Scripts->InvokeMethod(Victim, "OnDeath", { EntityOrNil(*Scene, Instigator) });
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->Death(*Scene, Victim, Instigator);
	}
	Systems.Scripts->BroadcastMethod("OnEntityDied", { EntityOrNil(*Scene, Victim), EntityOrNil(*Scene, Instigator) });

	FHealthComponent* Health = Registry.IsValid(Victim) ? Registry.TryGet<FHealthComponent>(Victim) : nullptr;
	if (Health == nullptr || !Gameplay::IsDead(*Health))
	{
		return; // 이벤트 처리 중 제거/되살림
	}
	const FEntity             CurrentGameMode = Gameplay::FindGameMode(*Scene);
	const FGameModeComponent* GameMode        = CurrentGameMode.IsValid() ? &Registry.Get<FGameModeComponent>(CurrentGameMode) : nullptr;

	EDeathAction Action = Health->DeathAction;
	if (Action == EDeathAction::Auto)
	{
		Action = GetOwner(Victim) >= 0 ? EDeathAction::RespawnAtPlayerStart : Action;
	}
	switch (Action)
	{
	case EDeathAction::Destroy:
		if (!Systems.Scripts->RequestDestroy(Victim))
		{
			Scene->DestroyEntity(Victim);
		}
		break;
	case EDeathAction::RespawnInPlace:
	case EDeathAction::RespawnAtPlayerStart:
	{
		const float Delay = GameMode != nullptr ? GameMode->RespawnDelay : FGameModeComponent{}.RespawnDelay;
		if (Delay >= 0.0f)
		{
			Health->Runtime.RespawnTimer = Delay; // 0이면 다음 틱
		}
		break;
	}
	default:
		break; // 그대로 둔다
	}
}

void FGameWorld::Respawn(FEntity Entity)
{
	FRegistry&        Registry = Scene->GetRegistry();
	FHealthComponent& Health   = Registry.Get<FHealthComponent>(Entity);
	Health.Health              = Health.MaxHealth;
	Health.Runtime.RespawnTimer = -1.0f;
	Health.Runtime.PendingEvents.clear();

	const bool bAtPlayerStart = Health.DeathAction == EDeathAction::RespawnAtPlayerStart || (Health.DeathAction == EDeathAction::Auto && GetOwner(Entity) >= 0);
	if (bAtPlayerStart)
	{
		const std::vector<FEntity> Starts = FNetPlayerSpawner::FindPlayerStarts(*Scene);
		if (!Starts.empty())
		{
			const FEntity        Start     = Starts[RespawnStartIndex++ % Starts.size()];
			FTransformComponent& Transform = Scene->GetTransform(Entity);
			Transform.Position             = Scene->GetTransform(Start).Position;
			Transform.Rotation             = Scene->GetTransform(Start).Rotation;
		}
	}
	if (FPhysicsSystem* Physics = Systems.Physics; Physics != nullptr)
	{
		if (Physics->HasCharacter(Entity))
		{
			Physics->SetCharacterState(*Scene, Entity, { Scene->GetTransform(Entity).Position, FVector3(), false });
		}
		else if (Physics->HasBody(Entity))
		{
			Physics->SetVelocity(Entity, FVector3()); // 위치 변경은 물리가 순간이동으로 처리한다
		}
	}
	Scene->UpdateTransforms();
	E_LOG(LogGameplay, Display, "리스폰: {}", DescribeEntity(*Scene, Entity));
	Systems.Scripts->InvokeMethod(Entity, "OnRespawned", {});
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->Respawned(*Scene, Entity);
	}
}

int32 FGameWorld::GetScoringPlayer(FEntity Instigator, FEntity Victim) const
{
	if (!Scene->GetRegistry().IsValid(Instigator) || Instigator == Victim)
	{
		return -1;
	}
	const int32 Owner = GetOwner(Instigator);
	if (Owner >= 0)
	{
		return Owner;
	}
	return Mode == ENetMode::Standalone ? GetLocalPlayerId() : -1; // 1인용: 소유자 없는 가해자 = 로컬 플레이어
}

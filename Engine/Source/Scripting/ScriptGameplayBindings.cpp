// Lua 게임플레이 바인딩: 체력/데미지, 게임 모드(Scene/Gameplay.h)
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>

// 체력 (서버/Standalone에서만 바뀐다 — 클라이언트에서는 아무것도 하지 않고 0/false)
//   entity:ApplyDamage(amount[, instigator]) → 깎인 양. 같은 프레임 게임플레이 단계에서 대상 스크립트 OnDamaged(amount, instigator),
//                                               죽었으면 OnDeath(instigator) + 모든 스크립트 OnEntityDied(victim, instigator)
//   entity:Heal(amount) → 회복한 양 (죽은 대상은 0)     entity:IsDead() → Health <= 0 (클라이언트는 복제 값)
//   값 읽기/쓰기는 리플렉션: entity:GetComponent("HealthComponent").Health
// 게임 모드 (씬의 첫 GameModeComponent, 없으면 "None"/0/-1)
//   GameMode.GetState() → "None" / "WaitingToStart" / "InProgress" / "Ended"   GameMode.GetTimeRemaining() → 초 (정수)
//   GameMode.GetScore(playerId) / GameMode.GetScores() → { [playerId] = score }   GameMode.GetWinner() → 플레이어 ID (-1 = 없음/무승부)
//   서버만: GameMode.AddScore(playerId, n), GameMode.StartMatch(), GameMode.EndMatch([winner]), GameMode.Restart()
//   상태가 바뀌면 모든 스크립트 OnMatchStateChanged(state) (클라이언트는 복제된 상태로)
void FLuaRuntime::RegisterGameplayBindings()
{
	const auto IsServer = [this]() { return NetHooks == nullptr || NetHooks->bIsServer; };
	const auto Valid    = [this](FEntity Entity) { return Scene != nullptr && Scene->GetRegistry().IsValid(Entity); };

	// ---- 체력
	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["ApplyDamage"] = [IsServer, Valid, this](const FScriptEntity& Entity, float Amount, sol::optional<FScriptEntity> Instigator) {
		if (!IsServer() || !Valid(Entity.Entity))
		{
			return 0.0f;
		}
		return Gameplay::ApplyDamage(*Scene, Entity.Entity, Amount, Instigator ? Instigator->Entity : FEntity());
	};
	EntityType["Heal"] = [IsServer, Valid, this](const FScriptEntity& Entity, float Amount) {
		return IsServer() && Valid(Entity.Entity) ? Gameplay::Heal(*Scene, Entity.Entity, Amount) : 0.0f;
	};
	EntityType["IsDead"] = [Valid, this](const FScriptEntity& Entity) { return Valid(Entity.Entity) && Gameplay::IsDead(*Scene, Entity.Entity); };

	// ---- 게임 모드
	const auto FindGameMode = [this]() -> FGameModeComponent* {
		if (Scene == nullptr)
		{
			return nullptr;
		}
		const FEntity Entity = Gameplay::FindGameMode(*Scene);
		return Entity.IsValid() ? Scene->GetRegistry().TryGet<FGameModeComponent>(Entity) : nullptr;
	};
	sol::table GameModeTable   = Lua.create_named_table("GameMode");
	GameModeTable["GetState"] = [FindGameMode]() {
		const FGameModeComponent* GameMode = FindGameMode();
		return std::string(GameMode != nullptr ? Gameplay::ToString(GameMode->MatchState) : "None");
	};
	GameModeTable["GetTimeRemaining"] = [FindGameMode]() {
		const FGameModeComponent* GameMode = FindGameMode();
		return GameMode != nullptr ? GameMode->RemainingSeconds : 0;
	};
	GameModeTable["GetWinner"] = [FindGameMode]() {
		const FGameModeComponent* GameMode = FindGameMode();
		return GameMode != nullptr ? GameMode->WinnerPlayerId : -1;
	};
	GameModeTable["GetScore"] = [FindGameMode](int32 PlayerId) {
		const FGameModeComponent* GameMode = FindGameMode();
		return GameMode != nullptr ? Gameplay::GetScore(*GameMode, PlayerId) : 0;
	};
	GameModeTable["GetScores"] = [FindGameMode, this]() {
		sol::table                Table    = Lua.create_table();
		const FGameModeComponent* GameMode = FindGameMode();
		if (GameMode != nullptr)
		{
			for (const auto& [Player, Score] : Gameplay::ParseScores(GameMode->Scores))
			{
				Table[Player] = Score;
			}
		}
		return Table;
	};
	GameModeTable["AddScore"] = [FindGameMode, IsServer](int32 PlayerId, int32 Amount) {
		if (FGameModeComponent* GameMode = FindGameMode(); GameMode != nullptr && IsServer())
		{
			Gameplay::AddScore(*GameMode, PlayerId, Amount);
		}
	};
	GameModeTable["StartMatch"] = [FindGameMode, IsServer]() {
		if (FGameModeComponent* GameMode = FindGameMode(); GameMode != nullptr && IsServer())
		{
			Gameplay::SetMatchState(*GameMode, EMatchState::InProgress);
		}
	};
	GameModeTable["EndMatch"] = [FindGameMode, IsServer](sol::optional<int32> Winner) {
		if (FGameModeComponent* GameMode = FindGameMode(); GameMode != nullptr && IsServer())
		{
			Gameplay::EndMatch(*GameMode, Winner ? *Winner : Gameplay::FindLeader(Gameplay::ParseScores(GameMode->Scores)));
		}
	};
	GameModeTable["Restart"] = [FindGameMode, IsServer]() {
		if (FGameModeComponent* GameMode = FindGameMode(); GameMode != nullptr && IsServer())
		{
			Gameplay::RestartMatch(*GameMode);
		}
	};
}

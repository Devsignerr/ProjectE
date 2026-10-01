// Lua 게임플레이 바인딩: 체력/데미지, 게임 모드(Scene/Gameplay.h), 세이브 게임(Core/SaveGame.h)
#include "Core/SaveGame.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

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
// 세이브 게임 (이 프로세스의 <Saved>/SaveGames — 클라이언트도 자기 컴퓨터에 저장)
//   SaveGame.Save(slot, table) → 성공 여부   SaveGame.Load(slot) → table 또는 nil   SaveGame.Exists(slot) / Delete(slot) / List()
//   값: nil/bool/number/string/table (배열 = 1..n 연속 키, 그 밖은 키를 문자열로). 객체 키는 불러오면 문자열이다
//   잘못된 슬롯 이름(경로 문자, '.', 공백 등)은 스크립트 오류
namespace
{
	constexpr int32 MaxSaveDepth = 32;

	void RequireSlot(const std::string& Slot)
	{
		if (!FSaveGame::IsValidSlotName(Slot))
		{
			throw std::runtime_error(std::format("SaveGame: 잘못된 슬롯 이름 \"{}\" (영문/숫자/'_'/'-'/한글, 1~{}바이트)", Slot, FSaveGame::MaxSlotNameLength));
		}
	}

	nlohmann::json ToJson(lua_State* L, const sol::object& Object, int32 Depth)
	{
		if (Depth > MaxSaveDepth)
		{
			throw std::runtime_error("SaveGame.Save: 테이블이 너무 깊습니다 (순환 참조?)");
		}
		switch (Object.get_type())
		{
		case sol::type::lua_nil: return nullptr;
		case sol::type::boolean: return Object.as<bool>();
		case sol::type::number:
		{
			Object.push(L);
			const bool bInteger = lua_isinteger(L, -1) != 0;
			const lua_Integer Integer = bInteger ? lua_tointeger(L, -1) : 0;
			const double      Number  = lua_tonumber(L, -1);
			lua_pop(L, 1);
			return bInteger ? nlohmann::json(static_cast<int64>(Integer)) : nlohmann::json(Number);
		}
		case sol::type::string: return Object.as<std::string>();
		case sol::type::table:
		{
			const sol::table Table = Object.as<sol::table>();
			// 1..n 연속 정수 키면 배열
			size_t Count     = 0;
			bool   bSequence = true;
			Table.for_each([&](const sol::object& Key, const sol::object&) {
				++Count;
				if (Key.get_type() != sol::type::number)
				{
					bSequence = false;
				}
			});
			for (size_t Index = 1; bSequence && Index <= Count; ++Index)
			{
				bSequence = Table.get<sol::object>(Index).get_type() != sol::type::lua_nil;
			}
			if (bSequence && Count > 0)
			{
				nlohmann::json Array = nlohmann::json::array();
				for (size_t Index = 1; Index <= Count; ++Index)
				{
					Array.push_back(ToJson(L, Table.get<sol::object>(Index), Depth + 1));
				}
				return Array;
			}
			nlohmann::json Result = nlohmann::json::object();
			Table.for_each([&](const sol::object& Key, const sol::object& Value) {
				std::string Name;
				if (Key.get_type() == sol::type::string)
				{
					Name = Key.as<std::string>();
				}
				else if (Key.get_type() == sol::type::number)
				{
					Key.push(L);
					Name = lua_isinteger(L, -1) ? std::to_string(lua_tointeger(L, -1)) : std::to_string(lua_tonumber(L, -1));
					lua_pop(L, 1);
				}
				else
				{
					throw std::runtime_error("SaveGame.Save: 테이블 키는 문자열이나 숫자만 저장할 수 있습니다");
				}
				Result[Name] = ToJson(L, Value, Depth + 1);
			});
			return Result;
		}
		default:
			throw std::runtime_error(std::format("SaveGame.Save: 저장할 수 없는 값입니다 ({})", sol::type_name(L, Object.get_type())));
		}
	}

	sol::object FromJson(sol::state& Lua, const nlohmann::json& Value)
	{
		switch (Value.type())
		{
		case nlohmann::json::value_t::boolean:         return sol::make_object(Lua, Value.get<bool>());
		case nlohmann::json::value_t::number_integer:  return sol::make_object(Lua, Value.get<int64>());
		case nlohmann::json::value_t::number_unsigned: return sol::make_object(Lua, static_cast<int64>(Value.get<uint64>()));
		case nlohmann::json::value_t::number_float:    return sol::make_object(Lua, Value.get<double>());
		case nlohmann::json::value_t::string:          return sol::make_object(Lua, Value.get<std::string>());
		case nlohmann::json::value_t::array:
		{
			sol::table Table = Lua.create_table(static_cast<int>(Value.size()), 0);
			for (size_t Index = 0; Index < Value.size(); ++Index)
			{
				Table[Index + 1] = FromJson(Lua, Value[Index]);
			}
			return Table;
		}
		case nlohmann::json::value_t::object:
		{
			sol::table Table = Lua.create_table(0, static_cast<int>(Value.size()));
			for (const auto& [Key, Item] : Value.items())
			{
				Table[Key] = FromJson(Lua, Item);
			}
			return Table;
		}
		default: return sol::make_object(Lua, sol::lua_nil);
		}
	}
} // namespace

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

	// ---- 세이브 게임
	sol::table SaveTable = Lua.create_named_table("SaveGame");
	SaveTable["Save"]    = [this](const std::string& Slot, sol::object Value) {
		RequireSlot(Slot);
		const nlohmann::json Root = ToJson(Lua.lua_state(), Value, 0);
		return FSaveGame::Save(Slot, Root.dump());
	};
	SaveTable["Load"] = [this](const std::string& Slot) -> sol::object {
		RequireSlot(Slot);
		const std::optional<std::string> Text = FSaveGame::Load(Slot);
		if (!Text)
		{
			return sol::make_object(Lua, sol::lua_nil);
		}
		return FromJson(Lua, nlohmann::json::parse(*Text, nullptr, false));
	};
	SaveTable["Exists"] = [](const std::string& Slot) {
		RequireSlot(Slot);
		return FSaveGame::Exists(Slot);
	};
	SaveTable["Delete"] = [](const std::string& Slot) {
		RequireSlot(Slot);
		return FSaveGame::Delete(Slot);
	};
	SaveTable["List"] = [this]() {
		sol::table Table = Lua.create_table();
		int32      Index = 1;
		for (const std::string& Slot : FSaveGame::List())
		{
			Table[Index++] = Slot;
		}
		return Table;
	};
}

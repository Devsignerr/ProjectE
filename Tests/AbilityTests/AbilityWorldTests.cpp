// 능력 시스템 + FGameWorld: Lua 능력 스크립트(코루틴 대기·노티파이 대기·취소), Lua API/이벤트, 루프백 서버·클라이언트 예측 확정/거절 되돌림
#include "AbilityTestCommon.h"

#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Ability/AbilitySystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneReflection.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	constexpr float Step = 1.0f / 30.0f;

	std::filesystem::path WriteScripts()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEAbilityTests";
		std::filesystem::create_directories(Directory / L"Abilities");
		std::filesystem::create_directories(Directory / L"Scripts");
		{
			std::ofstream File(Directory / L"Abilities/TestSpell.lua", std::ios::binary | std::ios::trunc);
			File << R"(
local Spell = { Properties = { Bonus = 5 } }
function Spell:OnActivate(ctx)
	SpellLog = (SpellLog or "") .. "start;"
	assert(self.Properties.Bonus == 5 and ctx.Ability == "Spell" and ctx.Owner == self.entity)
	ctx:ApplyEffectToSelf("Haste") -- 예측 창 안: 소유 클라이언트도 예측
	ctx:Wait(0.5)
	SpellLog = SpellLog .. "waited;"
	if ctx:HasAuthority() then
		ctx:ApplyEffectToTarget(Scene.Find("Dummy"), "Burn")
	else
		local Applied = ctx:ApplyEffectToTarget(Scene.Find("Dummy"), "Burn")
		assert(not Applied) -- 클라이언트는 다른 대상에 효과를 걸지 못한다
	end
	local Hit = ctx:WaitAnimNotify("Never", 0.2)
	SpellLog = SpellLog .. (Hit and "hit;" or "timeout;")
end
function Spell:OnEnd(ctx, cancelled)
	SpellLog = SpellLog .. (cancelled and "cancelled;" or "ended;")
end
return Spell
)";
		}
		{
			std::ofstream File(Directory / L"Scripts/AbilityRecorder.lua", std::ios::binary | std::ios::trunc);
			File << R"(
local R = { Properties = { Activated = "", Ended = "", Failed = "", Mana = -1, Tags = "" } }
function R:OnAbilityActivated(name) self.Properties.Activated = self.Properties.Activated .. name .. ";" end
function R:OnAbilityEnded(name, cancelled) self.Properties.Ended = self.Properties.Ended .. name .. (cancelled and "!" or "") .. ";" end
function R:OnAbilityFailed(name, reason) self.Properties.Failed = self.Properties.Failed .. name .. ":" .. reason .. ";" end
function R:OnAttributeChanged(name, value, old) if name == "Mana" then self.Properties.Mana = value end end
function R:OnTagChanged(tag, count) self.Properties.Tags = self.Properties.Tags .. tag .. "=" .. count .. ";" end
return R
)";
		}
		return Directory;
	}

	FEntity Find(FScene& Scene, const std::string& Name)
	{
		FEntity Result;
		Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Component) {
			if (Component.Name == Name)
			{
				Result = Entity;
			}
		});
		return Result;
	}

	float Attribute(FGameWorld& World, FScene& Scene, const char* Entity, const char* Name)
	{
		float Value = -1.0f;
		World.GetAbilities().GetAttribute(Find(Scene, Entity), Name, Value);
		return Value;
	}

	FEntity AddHero(FScene& Scene)
	{
		const FEntity     Hero   = Scene.CreateEntity("Hero");
		FScriptComponent& Script = Scene.GetRegistry().Emplace<FScriptComponent>(Hero);
		Script.ScriptAsset       = "Scripts/AbilityRecorder.lua";
		Script.ExecutionLocation = static_cast<int32>(EScriptExecution::Both);
		Scene.GetRegistry().Emplace<FAbilitySystemComponent>(Hero);
		const FEntity Dummy = Scene.CreateEntity("Dummy");
		Scene.GetRegistry().Emplace<FAbilitySystemComponent>(Dummy);
		Scene.GetTransform(Dummy).Position = FVector3(300.0f, 0.0f, 0.0f);
		Scene.UpdateTransforms();
		return Hero;
	}
} // namespace

// Standalone: Lua 능력 스크립트 = 코루틴 (Wait → 권한 효과 → WaitAnimNotify 제한 시간 → OnEnd), 발동 중 태그, 이벤트, Lua API, 취소
E_TEST(Ability_LuaAbilityScript)
{
	RegisterSceneTypes();
	const std::filesystem::path Content = WriteScripts();
	FScene                      Scene;
	const FEntity               Hero  = AddHero(Scene);
	const FEntity               Dummy = Find(Scene, "Dummy");

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	FAbilitySystem& Abilities = World.GetAbilities();
	const auto      Set       = AbilityTest::MakeSet("Abilities/TestSpell.lua");
	E_EXPECT_TRUE(Abilities.InitializeWithSet(Hero, Set));
	E_EXPECT_TRUE(Abilities.InitializeWithSet(Dummy, Set));
	World.TickGameplay(Step, nullptr); // 스크립트 OnStart

	E_EXPECT_TRUE(Scripts.RunString("local Ok, Why = Scene.Find('Hero'):TryActivateAbility('Spell'); assert(Ok, Why)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(SpellLog == 'start;')"));
	E_EXPECT_TRUE(Abilities.HasTag(Hero, "State.Casting"));
	E_EXPECT_TRUE(Abilities.HasTag(Hero, "State.Hasted"));
	E_EXPECT_NEAR(Attribute(World, Scene, "Hero", "Mana"), 80.0f, 1.0e-3f);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Hero = Scene.Find('Hero')
assert(Hero:HasAbilitySystem() and Hero:IsAbilityActive('Spell'))
assert(Hero:GetAttribute('Mana') == 80 and Hero:GetBaseAttribute('MoveSpeed') == 400 and Hero:GetAttribute('Nope') == nil)
assert(Hero:HasTag('State') and Hero:HasTagExact('State.Casting') and Hero:GetTagCount('Ability.Cooldown') == 1)
local Remaining, Duration = Hero:GetAbilityCooldown('Spell')
assert(Remaining > 1.9 and Duration == 2)
local Ok, Why = Hero:TryActivateAbility('Spell')
assert(not Ok and Why == 'Active')
local Names = Hero:GetAbilities()
assert(#Names == 4 and Names[4] == 'Spell')
local Near = Abilities.FindInRadius(Vector3(0, 0, 0), 500)
assert(#Near == 2 and Near[1] == Hero)
assert(#Abilities.FindInRadius(Vector3(0, 0, 0), 100, Hero) == 0)
)"));

	for (int32 Frame = 0; Frame < 18; ++Frame) // 0.6초
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Scripts.RunString("assert(SpellLog == 'start;waited;', SpellLog)"));
	E_EXPECT_TRUE(Abilities.HasTag(Dummy, "State.Burning"));
	for (int32 Frame = 0; Frame < 9; ++Frame) // 0.3초 — 노티파이 대기 제한(0.2초) 넘김
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Scripts.RunString("assert(SpellLog == 'start;waited;timeout;ended;', SpellLog)"));
	E_EXPECT_FALSE(Abilities.IsAbilityActive(Hero, "Spell"));
	E_EXPECT_FALSE(Abilities.HasTag(Hero, "State.Casting"));
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Activated").String == "Spell;");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Ended").String == "Spell;");
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Hero, "Mana").Number, 80.0, 1.0e-6);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Tags").String.find("State.Casting=0;") != std::string::npos);

	// 쿨다운 중 실패 이벤트 → 쿨다운 끝 → 발동 후 취소 (OnEnd cancelled)
	E_EXPECT_TRUE(Scripts.RunString("assert(not Scene.Find('Hero'):TryActivateAbility('Spell'))"));
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Failed").String == "Spell:Active;Spell:Cooldown;");
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Scripts.RunString("SpellLog = ''; assert(Scene.Find('Hero'):TryActivateAbility('Spell')); assert(Scene.Find('Hero'):CancelAbility('Spell'))"));
	E_EXPECT_TRUE(Scripts.RunString("assert(SpellLog == 'start;cancelled;', SpellLog)"));

	// Lua 효과 API (서버): SetByCaller, 활성 효과 목록, 핸들로 제거, 느슨한 태그
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Dummy = Scene.Find('Dummy')
local Ok = Dummy:ApplyEffect('Damage', { Source = Scene.Find('Hero'), SetByCaller = { Damage = 12 } })
assert(Ok and Dummy:GetAttribute('Health') < 90)
local Ok2, Handle = Dummy:ApplyEffect('Shield')
assert(Ok2 and Handle > 0 and Dummy:HasTag('State.Shielded'))
local Found = false
for _, E in ipairs(Dummy:GetActiveEffects()) do if E.Name == 'Shield' then Found = E.Remaining > 4 and E.Stacks == 1 end end
assert(Found)
assert(Dummy:RemoveEffect(Handle) and not Dummy:HasTag('State.Shielded'))
assert(Dummy:AddLooseTag('State.Marked') and Dummy:HasTag('State.Marked') and Dummy:RemoveLooseTag('State.Marked'))
assert(Dummy:SetBaseAttribute('Mana', 33) and Dummy:GetAttribute('Mana') == 33)
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 루프백 전용 서버 + 클라이언트: 소유 클라이언트 예측(비용·쿨다운·발동 태그·자기 효과·로컬 스크립트) → 서버 확정 후 합쳐도 중복 없음.
// 서버가 먼저 기절시킨 상태에서 예측 발동 → 거절 → 예측 되돌림 + OnAbilityFailed("Rejected:Blocked") + 로컬 스크립트 취소
E_TEST(Ability_PredictionConfirmAndReject)
{
	RegisterSceneTypes();
	RegisterNetworkTypes();
	const std::filesystem::path Content = WriteScripts();
	auto                        Hub     = std::make_shared<FLoopbackHub>();
	FNetSessionInfo             Session;
	Session.ProjectName = "AbilityTest";
	const auto Set      = AbilityTest::MakeSet("Abilities/TestSpell.lua");
	const auto Build    = [](FScene& Scene) {
		const FEntity Hero = AddHero(Scene);
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Hero);
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Find(Scene, "Dummy"));
	};

	FScene             ServerScene;
	FScriptSystem      ServerScripts;
	FNetDriver         ServerNet;
	FGameWorld         ServerWorld;
	FReplicationServer ServerReplication;
	Build(ServerScene);
	ServerWorld.Init({ &ServerScripts, nullptr, nullptr, nullptr, Content, &ServerNet });
	ServerNet.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
	ServerReplication.Begin(ServerScene, ServerNet);
	ServerNet.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) { ServerReplication.OnPlayerJoined(Player.Connection); };
	ServerNet.OnGameMessage  = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { ServerWorld.HandleNetMessage(Connection, Message); };
	ServerWorld.BeginPlay(ServerScene, ENetMode::DedicatedServer);
	ServerWorld.GetAbilities().InitializeWithSet(Find(ServerScene, "Hero"), Set);
	ServerWorld.GetAbilities().InitializeWithSet(Find(ServerScene, "Dummy"), Set);

	FScene             ClientScene;
	FScriptSystem      ClientScripts;
	FNetDriver         ClientNet;
	FGameWorld         ClientWorld;
	FReplicationClient ClientReplication;
	Build(ClientScene);
	ClientWorld.Init({ &ClientScripts, nullptr, nullptr, nullptr, Content, &ClientNet });
	ClientReplication.Begin(ClientScene);
	ClientNet.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
		if (!ClientReplication.HandleMessage(Message))
		{
			ClientWorld.HandleNetMessage(Connection, Message);
		}
	};
	ClientWorld.BeginPlay(ClientScene, ENetMode::Client);
	ClientWorld.GetAbilities().InitializeWithSet(Find(ClientScene, "Hero"), Set);
	ClientWorld.GetAbilities().InitializeWithSet(Find(ClientScene, "Dummy"), Set);
	ClientNet.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Session);

	const auto Pump = [&](int32 Rounds) {
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			ServerNet.Update(Step);
			ServerWorld.TickGameplay(Step, nullptr);
			ServerReplication.Tick(Step);
			ClientNet.Update(Step);
			ClientReplication.Update(Step);
			ClientWorld.TickGameplay(Step, nullptr);
		}
	};
	Pump(6);
	E_EXPECT_TRUE(ClientNet.GetClientState() == FNetDriver::EClientState::Joined);
	const FEntity ServerHero = Find(ServerScene, "Hero");
	const FEntity ClientHero = Find(ClientScene, "Hero");
	ServerScene.GetRegistry().Get<FReplicatedComponent>(ServerHero).OwnerPlayerId = static_cast<int32>(ClientNet.GetLocalPlayerId());
	Pump(6);
	E_EXPECT_EQ(ClientScene.GetRegistry().Get<FReplicatedComponent>(ClientHero).OwnerPlayerId, static_cast<int32>(ClientNet.GetLocalPlayerId()));
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "Mana"), 100.0f, 1.0e-3f); // 서버 복제 값

	// 1) 예측 발동: 클라이언트는 즉시, 서버는 아직
	E_EXPECT_TRUE(ClientScripts.RunString("local Ok, Why = Scene.Find('Hero'):TryActivateAbility('Spell'); assert(Ok, Why)"));
	E_EXPECT_TRUE(ClientScripts.RunString("assert(SpellLog == 'start;')"));
	FAbilitySystem& Client = ClientWorld.GetAbilities();
	FAbilitySystem& Server = ServerWorld.GetAbilities();
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "Mana"), 80.0f, 1.0e-3f);
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "MoveSpeed"), 675.0f, 1.0e-3f); // 예측 Haste
	E_EXPECT_TRUE(Client.HasTag(ClientHero, "State.Casting") && Client.HasTag(ClientHero, "Ability.Cooldown.Spell"));
	E_EXPECT_NEAR(Attribute(ServerWorld, ServerScene, "Hero", "Mana"), 100.0f, 1.0e-3f);

	// 2) 서버 확정 → 복제: 예측을 버리고 서버 값만 (중복 적용 없음)
	Pump(4);
	E_EXPECT_NEAR(Attribute(ServerWorld, ServerScene, "Hero", "Mana"), 80.0f, 1.0e-3f);
	E_EXPECT_TRUE(Server.IsAbilityActive(ServerHero, "Spell"));
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "Mana"), 80.0f, 1.0e-3f);
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "MoveSpeed"), 675.0f, 1.0e-3f);
	int32 Cooldowns = 0;
	int32 Predicted = 0;
	for (const FActiveEffectView& Effect : Client.GetActiveEffects(ClientHero))
	{
		Cooldowns += Effect.Name == "Cooldown_Spell" ? 1 : 0;
		Predicted += Effect.bPredicted ? 1 : 0;
	}
	E_EXPECT_EQ(Cooldowns, 1);
	E_EXPECT_EQ(Predicted, 0);
	E_EXPECT_EQ(ClientScene.GetRegistry().Get<FAbilitySystemComponent>(ClientHero).RepPredictionKey, 1);
	Pump(30); // 1초: 양쪽 스크립트 끝, 서버만 Dummy에 화상
	E_EXPECT_TRUE(ClientScripts.RunString("assert(SpellLog == 'start;waited;timeout;ended;', SpellLog)"));
	E_EXPECT_TRUE(ServerScripts.RunString("assert(SpellLog == 'start;waited;timeout;ended;', SpellLog)"));
	E_EXPECT_TRUE(Client.HasTag(Find(ClientScene, "Dummy"), "State.Burning")); // 복제된 태그
	E_EXPECT_FALSE(Client.HasTag(ClientHero, "State.Casting"));

	// 3) 거절: 서버가 기절시킨 직후(클라이언트는 아직 모름) 예측 발동
	Pump(45); // 쿨다운(2초) 끝
	E_EXPECT_FALSE(Client.HasTag(ClientHero, "Ability.Cooldown.Spell"));
	E_EXPECT_TRUE(Server.ApplyEffect(ServerHero, "Stun").bApplied);
	E_EXPECT_TRUE(ClientScripts.RunString("SpellLog = ''; assert(Scene.Find('Hero'):TryActivateAbility('Spell'))"));
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "Mana"), 60.0f, 1.0e-3f);
	Pump(4);
	E_EXPECT_EQ(Client.GetRejectedCount(), 1u);
	E_EXPECT_TRUE(Client.GetRolledBackCount() > 0u);
	E_EXPECT_NEAR(Attribute(ServerWorld, ServerScene, "Hero", "Mana"), 80.0f, 1.0e-3f);
	E_EXPECT_NEAR(Attribute(ClientWorld, ClientScene, "Hero", "Mana"), 80.0f, 1.0e-3f);
	E_EXPECT_FALSE(Client.HasTag(ClientHero, "Ability.Cooldown.Spell"));
	E_EXPECT_FALSE(Client.IsAbilityActive(ClientHero, "Spell"));
	E_EXPECT_TRUE(ClientScripts.RunString("assert(SpellLog == 'start;cancelled;', SpellLog)"));
	E_EXPECT_TRUE(ClientScripts.GetInstanceProperty(ClientHero, "Failed").String == "Spell:Rejected:Blocked;");
	E_EXPECT_TRUE(Client.HasTag(ClientHero, "State.Stunned")); // 그 사이 기절도 복제됨
	// 클라이언트 쪽 권한 API는 아무것도 하지 않는다
	E_EXPECT_TRUE(ClientScripts.RunString("assert(not Scene.Find('Dummy'):ApplyEffect('Burn')); assert(not Scene.Find('Hero'):SetBaseAttribute('Mana', 1))"));
	E_EXPECT_EQ(ServerScripts.GetErrorCount(), 0u);
	E_EXPECT_EQ(ClientScripts.GetErrorCount(), 0u);

	ClientWorld.EndPlay();
	ServerWorld.EndPlay();
}

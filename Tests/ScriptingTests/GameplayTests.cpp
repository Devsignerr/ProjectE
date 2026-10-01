#include "Core/SaveGame.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	constexpr float Step = 1.0f / 30.0f;

	// 게임플레이 이벤트를 Properties에 기록하는 스크립트 (Both)
	std::filesystem::path WriteRecorderScript()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEGameplayTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/Recorder.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Damaged = 0, Instigator = "", Respawns = 0, Died = "", State = "", ClientState = "", ClientScore = -1 } }
function T:OnDamaged(amount, instigator)
	self.Properties.Damaged    = self.Properties.Damaged + amount
	self.Properties.Instigator = instigator and instigator:GetName() or "nil"
end
function T:OnDeath(instigator) Deaths = (Deaths or 0) + 1; LastKiller = instigator and instigator:GetName() or "nil" end
function T:OnRespawned() self.Properties.Respawns = self.Properties.Respawns + 1 end
function T:OnEntityDied(victim, instigator) self.Properties.Died = self.Properties.Died .. victim:GetName() .. ";" end
function T:OnMatchStateChanged(state) self.Properties.State = self.Properties.State .. state .. ";" end
function T:OnUpdate(dt)
	self.Properties.ClientState = GameMode.GetState()
	self.Properties.ClientScore = GameMode.GetScore(1)
end
return T
)";
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

	FEntity AddRecorded(FScene& Scene, const char* Name)
	{
		const FEntity     Entity = Scene.CreateEntity(Name);
		FScriptComponent& Script = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
		Script.ScriptAsset       = "Scripts/Recorder.lua";
		Script.ExecutionLocation = static_cast<int32>(EScriptExecution::Both);
		return Entity;
	}

	struct FCountingModule final : IGameModule
	{
		int32 Damaged   = 0;
		int32 Deaths    = 0;
		int32 Respawned = 0;
		float Amount    = 0.0f;
		void  OnDamaged(FScene&, FEntity, float InAmount, FEntity) override { ++Damaged; Amount += InAmount; }
		void  OnDeath(FScene&, FEntity, FEntity) override { ++Deaths; }
		void  OnRespawned(FScene&, FEntity) override { ++Respawned; }
	};
} // namespace

// 순수 규칙: 데미지는 남은 체력까지만, 무적/죽음/비정상 값은 무시, 회복은 최대 체력까지 (죽은 대상은 안 됨)
E_TEST(Gameplay_HealthRules)
{
	FHealthComponent Health;
	Health.MaxHealth = 100.0f;
	Health.Health    = 100.0f;
	bool bKilled     = false;
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Health, 30.0f, bKilled), 30.0f, 1.0e-5f);
	E_EXPECT_FALSE(bKilled);
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Health, -5.0f, bKilled), 0.0f, 1.0e-5f);
	E_EXPECT_NEAR(Gameplay::Heal(Health, 50.0f), 30.0f, 1.0e-5f); // 최대 100까지
	Health.bInvulnerable = true;
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Health, 10.0f, bKilled), 0.0f, 1.0e-5f);
	Health.bInvulnerable = false;
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Health, 250.0f, bKilled), 100.0f, 1.0e-5f);
	E_EXPECT_TRUE(bKilled);
	E_EXPECT_TRUE(Gameplay::IsDead(Health));
	E_EXPECT_NEAR(Health.Health, 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Health, 10.0f, bKilled), 0.0f, 1.0e-5f); // 죽은 대상
	E_EXPECT_FALSE(bKilled);
	E_EXPECT_NEAR(Gameplay::Heal(Health, 10.0f), 0.0f, 1.0e-5f);
}

// 매치 흐름: 대기 → (StartDelay) 진행 → 목표 점수 / 제한 시간 → 끝. 점수 문자열 왕복, 공동 1위 = 승자 없음
E_TEST(Gameplay_MatchFlow)
{
	E_EXPECT_TRUE(Gameplay::FormatScores(Gameplay::ParseScores("2=5;0=3;bad;x=1")) == "0=3;2=5");
	E_EXPECT_EQ(Gameplay::FindLeader(Gameplay::ParseScores("0=3;2=5")), 2);
	E_EXPECT_EQ(Gameplay::FindLeader(Gameplay::ParseScores("0=5;2=5")), -1);
	E_EXPECT_EQ(Gameplay::FindLeader({}), -1);

	FGameModeComponent GameMode;
	GameMode.StartDelay = 1.0f;
	GameMode.ScoreToWin = 3;
	GameMode.TimeLimit  = 10.0f;
	E_EXPECT_FALSE(Gameplay::TickMatch(GameMode, 0.5f));
	E_EXPECT_TRUE(GameMode.MatchState == EMatchState::WaitingToStart);
	E_EXPECT_TRUE(Gameplay::TickMatch(GameMode, 0.6f));
	E_EXPECT_TRUE(GameMode.MatchState == EMatchState::InProgress);
	E_EXPECT_EQ(GameMode.RemainingSeconds, 10);
	Gameplay::TickMatch(GameMode, 2.5f);
	E_EXPECT_EQ(GameMode.RemainingSeconds, 8); // 7.5초 → 올림
	Gameplay::AddScore(GameMode, 1, 2);
	Gameplay::AddScore(GameMode, 4, 1);
	Gameplay::AddScore(GameMode, -1, 9); // 플레이어 아님
	E_EXPECT_FALSE(Gameplay::TickMatch(GameMode, 0.1f));
	Gameplay::AddScore(GameMode, 1, 1);
	E_EXPECT_TRUE(Gameplay::TickMatch(GameMode, 0.1f));
	E_EXPECT_TRUE(GameMode.MatchState == EMatchState::Ended);
	E_EXPECT_EQ(GameMode.WinnerPlayerId, 1);
	E_EXPECT_EQ(Gameplay::GetScore(GameMode, 1), 3);

	// 제한 시간 종료: 최고 점수 (동점이면 -1)
	Gameplay::RestartMatch(GameMode);
	E_EXPECT_TRUE(GameMode.MatchState == EMatchState::WaitingToStart && GameMode.Scores.empty());
	Gameplay::TickMatch(GameMode, 1.0f);
	E_EXPECT_TRUE(GameMode.MatchState == EMatchState::InProgress);
	Gameplay::AddScore(GameMode, 0, 1);
	Gameplay::AddScore(GameMode, 2, 1);
	E_EXPECT_TRUE(Gameplay::TickMatch(GameMode, 10.0f));
	E_EXPECT_TRUE(GameMode.MatchState == EMatchState::Ended);
	E_EXPECT_EQ(GameMode.WinnerPlayerId, -1);
	E_EXPECT_EQ(GameMode.RemainingSeconds, 0);
}

// Standalone 월드: 데미지 이벤트(스크립트 + 게임 모듈) → 사망(파괴, 점수) / 리스폰(PlayerStart, 체력 회복), 죽은 대상 무시
E_TEST(Gameplay_DamageDeathAndRespawn)
{
	RegisterNetworkTypes();
	const std::filesystem::path Content = WriteRecorderScript();
	FScene                      Scene;
	const FEntity               Start = Scene.CreateEntity(FNetPlayerSpawner::PlayerStartName);
	Scene.GetTransform(Start).Position = FVector3(500.0f, 0.0f, 100.0f);

	const FEntity GameModeEntity = Scene.CreateEntity("GameMode");
	FGameModeComponent& GameModeInit = Scene.GetRegistry().Emplace<FGameModeComponent>(GameModeEntity);
	GameModeInit.RespawnDelay        = 0.5f;
	GameModeInit.ScoreToWin          = 4;

	AddRecorded(Scene, "Shooter");
	const FEntity     Target       = AddRecorded(Scene, "Target");
	FHealthComponent& TargetHealth = Scene.GetRegistry().Emplace<FHealthComponent>(Target);
	TargetHealth.MaxHealth = TargetHealth.Health = 30.0f;
	TargetHealth.DeathAction                     = EDeathAction::Destroy;
	TargetHealth.ScoreValue                      = 2;

	const FEntity Player = AddRecorded(Scene, "Player");
	Scene.GetRegistry().Emplace<FHealthComponent>(Player).DeathAction = EDeathAction::RespawnAtPlayerStart;
	Scene.GetTransform(Player).Position                              = FVector3(-300.0f, 0.0f, 0.0f);
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FCountingModule Module;
	FGameModuleHost Host;
	Host.Attach(Module, "GameplayTestModule");
	FGameWorld World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(Step, nullptr);
	const FEntity Shooter = Find(Scene, "Shooter");
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FGameModeComponent>(GameModeEntity).MatchState == EMatchState::InProgress);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Shooter, "State").String == "InProgress;"); // 대기 0초 → 첫 틱에 진행

	// 데미지: 값은 바로, 이벤트는 같은 틱 게임플레이 단계에서
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('Target'):ApplyDamage(20, Scene.Find('Shooter')) == 20)"));
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Target).Health, 10.0f, 1.0e-5f);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Target, "Damaged").Number, 20.0, 1.0e-6);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Target, "Instigator").String == "Shooter");
	E_EXPECT_EQ(Module.Damaged, 1);

	// 사망 → OnDeath, OnEntityDied(모두), 점수(Standalone: 소유자 없는 가해자 = 플레이어 0), 파괴
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('Target'):ApplyDamage(50, Scene.Find('Shooter')) == 10)"));
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(Deaths == 1 and LastKiller == 'Shooter')"));
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Shooter, "Died").String == "Target;");
	E_EXPECT_EQ(Module.Deaths, 1);
	E_EXPECT_NEAR(Module.Amount, 30.0f, 1.0e-5f);
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Target)); // Destroy (스크립트 지연 파괴)
	E_EXPECT_EQ(Gameplay::GetScore(Scene.GetRegistry().Get<FGameModeComponent>(GameModeEntity), 0), 2);

	// 플레이어: C++ 데미지로 사망 → 지연 뒤 PlayerStart에서 체력 회복
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Scene, Player, 1000.0f, Shooter), 100.0f, 1.0e-5f);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Gameplay::IsDead(Scene, Player));
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('Player'):IsDead())"));
	E_EXPECT_NEAR(Gameplay::ApplyDamage(Scene, Player, 10.0f, Shooter), 0.0f, 1.0e-6f); // 죽은 대상
	for (int32 Frame = 0; Frame < 10; ++Frame) // 0.33초 — 아직
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Gameplay::IsDead(Scene, Player));
	for (int32 Frame = 0; Frame < 8; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_FALSE(Gameplay::IsDead(Scene, Player));
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FHealthComponent>(Player).Health, 100.0f, 1.0e-5f);
	E_EXPECT_EQUALS(Scene.GetTransform(Player).Position, FVector3(500.0f, 0.0f, 100.0f), 1.0e-3f);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Player, "Respawns").Number, 1.0, 1.0e-9);
	E_EXPECT_EQ(Module.Respawned, 1);
	E_EXPECT_EQ(Gameplay::GetScore(Scene.GetRegistry().Get<FGameModeComponent>(GameModeEntity), 0), 2); // 플레이어 ScoreValue 0

	// 게임 모드 API: 점수로 승리 → OnMatchStateChanged("Ended")
	E_EXPECT_TRUE(Scripts.RunString("GameMode.AddScore(0, 2); assert(GameMode.GetScores()[0] == 4)"));
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(GameMode.GetState() == 'Ended' and GameMode.GetWinner() == 0)"));
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Shooter, "State").String == "InProgress;Ended;");
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
	Host.Unload();
}

// 소유 플레이어 폰(Auto)은 PlayerStart에서 리스폰. 게임 모드가 없으면 기본 지연(3초), RespawnInPlace는 제자리
E_TEST(Gameplay_AutoRespawnForPlayerPawn)
{
	RegisterNetworkTypes();
	FScene        Scene;
	const FEntity Start                = Scene.CreateEntity(FNetPlayerSpawner::PlayerStartName);
	Scene.GetTransform(Start).Position = FVector3(0.0f, 700.0f, 50.0f);
	const FEntity Pawn                 = Scene.CreateEntity("Pawn");
	Scene.GetRegistry().Emplace<FReplicatedComponent>(Pawn).OwnerPlayerId = 0;
	Scene.GetRegistry().Emplace<FHealthComponent>(Pawn);
	const FEntity Crate = Scene.CreateEntity("Crate");
	Scene.GetRegistry().Emplace<FHealthComponent>(Crate).DeathAction = EDeathAction::RespawnInPlace;
	Scene.GetTransform(Crate).Position                               = FVector3(100.0f, 0.0f, 0.0f);
	const FEntity Rock                                               = Scene.CreateEntity("Rock");
	Scene.GetRegistry().Emplace<FHealthComponent>(Rock); // Auto + 소유자 없음 = 그대로
	Scene.UpdateTransforms();

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.BeginPlay(Scene);
	for (FEntity Entity : { Pawn, Crate, Rock })
	{
		Gameplay::ApplyDamage(Scene, Entity, 500.0f, FEntity());
	}
	for (int32 Frame = 0; Frame < 80; ++Frame) // 2.67초
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Gameplay::IsDead(Scene, Pawn));
	for (int32 Frame = 0; Frame < 12; ++Frame) // 3초 넘김
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_FALSE(Gameplay::IsDead(Scene, Pawn));
	E_EXPECT_EQUALS(Scene.GetTransform(Pawn).Position, FVector3(0.0f, 700.0f, 50.0f), 1.0e-3f);
	E_EXPECT_FALSE(Gameplay::IsDead(Scene, Crate));
	E_EXPECT_EQUALS(Scene.GetTransform(Crate).Position, FVector3(100.0f, 0.0f, 0.0f), 1.0e-3f);
	E_EXPECT_TRUE(Gameplay::IsDead(Scene, Rock));
	E_EXPECT_TRUE(Scene.GetRegistry().IsValid(Rock));
	World.EndPlay();
}

// 전용 서버 + 클라이언트 (루프백): 체력·게임 상태(매치 상태/점수/승자)가 복제되고, 클라이언트 스크립트가 OnMatchStateChanged를 받는다.
// 클라이언트의 entity:ApplyDamage/GameMode.AddScore는 아무것도 하지 않는다
E_TEST(Gameplay_StateReplicatesToClient)
{
	RegisterNetworkTypes();
	const std::filesystem::path Content = WriteRecorderScript();
	auto                        Hub     = std::make_shared<FLoopbackHub>();
	FNetSessionInfo             Session;
	Session.ProjectName = "Test";

	const auto BuildLevel = [](FScene& Scene) {
		const FEntity       GameModeEntity = AddRecorded(Scene, "GameMode");
		FGameModeComponent& GameMode       = Scene.GetRegistry().Emplace<FGameModeComponent>(GameModeEntity);
		GameMode.StartDelay                = 0.3f;
		GameMode.ScoreToWin                = 3;
		Scene.GetRegistry().Emplace<FReplicatedComponent>(GameModeEntity);
		const FEntity Target = Scene.CreateEntity("Target");
		Scene.GetRegistry().Emplace<FHealthComponent>(Target).DeathAction = EDeathAction::RespawnInPlace;
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Target);
		Scene.UpdateTransforms();
	};

	FScene             ServerScene;
	FScriptSystem      ServerScripts;
	FNetDriver         ServerNet;
	FGameWorld         ServerWorld;
	FReplicationServer ServerReplication;
	BuildLevel(ServerScene);
	ServerWorld.Init({ &ServerScripts, nullptr, nullptr, nullptr, Content, &ServerNet });
	ServerNet.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
	ServerReplication.Begin(ServerScene, ServerNet);
	ServerNet.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) { ServerReplication.OnPlayerJoined(Player.Connection); };
	ServerNet.OnGameMessage  = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { ServerWorld.HandleNetMessage(Connection, Message); };
	ServerWorld.BeginPlay(ServerScene, ENetMode::DedicatedServer);

	FScene             ClientScene;
	FScriptSystem      ClientScripts;
	FNetDriver         ClientNet;
	FGameWorld         ClientWorld;
	FReplicationClient ClientReplication;
	BuildLevel(ClientScene);
	ClientWorld.Init({ &ClientScripts, nullptr, nullptr, nullptr, Content, &ClientNet });
	ClientReplication.Begin(ClientScene);
	ClientNet.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
		if (!ClientReplication.HandleMessage(Message))
		{
			ClientWorld.HandleNetMessage(Connection, Message);
		}
	};
	ClientWorld.BeginPlay(ClientScene, ENetMode::Client);
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
	const FEntity ClientGameMode = Find(ClientScene, "GameMode");
	const FEntity ServerGameMode = Find(ServerScene, "GameMode");
	E_EXPECT_TRUE(ClientScene.GetRegistry().Get<FGameModeComponent>(ClientGameMode).MatchState == EMatchState::WaitingToStart);
	Pump(6); // StartDelay 0.3초 지남
	E_EXPECT_TRUE(ClientScene.GetRegistry().Get<FGameModeComponent>(ClientGameMode).MatchState == EMatchState::InProgress);

	// 클라이언트는 데미지/점수를 바꾸지 못한다
	E_EXPECT_TRUE(ClientScripts.RunString("assert(Scene.Find('Target'):ApplyDamage(40) == 0); GameMode.AddScore(1, 5)"));
	E_EXPECT_NEAR(ClientScene.GetRegistry().Get<FHealthComponent>(Find(ClientScene, "Target")).Health, 100.0f, 1.0e-5f);

	// 서버 데미지/점수 → 복제
	E_EXPECT_TRUE(ServerScripts.RunString("Scene.Find('Target'):ApplyDamage(40); GameMode.AddScore(1, 3)"));
	Pump(6);
	E_EXPECT_NEAR(ClientScene.GetRegistry().Get<FHealthComponent>(Find(ClientScene, "Target")).Health, 60.0f, 1.0e-5f);
	const FGameModeComponent& ClientState = ClientScene.GetRegistry().Get<FGameModeComponent>(ClientGameMode);
	E_EXPECT_TRUE(ClientState.MatchState == EMatchState::Ended);
	E_EXPECT_EQ(ClientState.WinnerPlayerId, 1);
	E_EXPECT_TRUE(ClientState.Scores == "1=3");
	E_EXPECT_TRUE(ClientScripts.GetInstanceProperty(ClientGameMode, "State").String == "WaitingToStart;InProgress;Ended;");
	E_EXPECT_TRUE(ClientScripts.GetInstanceProperty(ClientGameMode, "ClientState").String == "Ended");
	E_EXPECT_NEAR(ClientScripts.GetInstanceProperty(ClientGameMode, "ClientScore").Number, 3.0, 1.0e-9);
	E_EXPECT_TRUE(ServerScripts.GetInstanceProperty(ServerGameMode, "State").String == "WaitingToStart;InProgress;Ended;");
	E_EXPECT_EQ(ServerScripts.GetErrorCount(), 0u);
	E_EXPECT_EQ(ClientScripts.GetErrorCount(), 0u);

	ClientWorld.EndPlay();
	ServerWorld.EndPlay();
}

// Lua SaveGame: 테이블 왕복 (중첩/배열/숫자 정수·실수), 목록/삭제, 잘못된 슬롯 이름은 스크립트 오류
E_TEST(Gameplay_LuaSaveGameRoundTrip)
{
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEGameplaySaveGames";
	std::filesystem::remove_all(Directory);
	FSaveGame::SetDirectoryOverride(Directory);

	FScene        Scene;
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.BeginPlay(Scene);
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(SaveGame.Save("Slot_1", { Level = 3, Name = "용사", Ratio = 0.5, Items = { "검", "방패" }, Flags = { Boss = true }, [7] = "seven" }))
assert(SaveGame.Exists("Slot_1") and not SaveGame.Exists("Other"))
local Data = SaveGame.Load("Slot_1")
assert(Data.Level == 3 and math.type(Data.Level) == "integer")
assert(Data.Name == "용사" and Data.Ratio == 0.5)
assert(#Data.Items == 2 and Data.Items[2] == "방패")
assert(Data.Flags.Boss == true and Data["7"] == "seven")
assert(SaveGame.Load("Missing") == nil)
assert(SaveGame.Save("Slot_2", {}))
local Slots = SaveGame.List()
assert(#Slots == 2 and Slots[1] == "Slot_1" and Slots[2] == "Slot_2")
assert(SaveGame.Delete("Slot_2") and not SaveGame.Delete("Slot_2"))
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_FALSE(Scripts.RunString("SaveGame.Save('../Escape', { })"));
	E_EXPECT_FALSE(Scripts.RunString("SaveGame.Save('Bad', { f = function() end })"));
	E_EXPECT_FALSE(std::filesystem::exists(Directory.parent_path() / L"Escape.json"));
	World.EndPlay();
	FSaveGame::SetDirectoryOverride({});
}

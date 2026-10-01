// 게임 중 맵 전환 (Phase 31-1): Lua Game.OpenScene / 게임 모듈 OpenScene, 씬 사이 값(Game.SetPersistent), 서버 이동에 클라이언트가 따라오기
#include "Core/StringConv.h"
#include "Core/Testing/TestFramework.h"
#include "Editor/EditorContext.h"
#include "Editor/PlayMode.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/GameModule.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"
#include "World/GameWorldTravel.h"

#include <filesystem>
#include <fstream>

namespace
{
	FEntity FindNamed(FScene& Scene, const std::string& Name)
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

	int32 CountPawns(FScene& Scene, int32* OutOwnedBy = nullptr, int32 Owner = -1)
	{
		int32 Count = 0;
		Scene.GetRegistry().View<FReplicatedComponent>().Each([&](FEntity Entity, FReplicatedComponent& Replicated) {
			if (FPrefabLibrary::IsInstanceRoot(Scene, Entity))
			{
				++Count;
				if (OutOwnedBy != nullptr && Replicated.OwnerPlayerId == Owner)
				{
					++*OutOwnedBy;
				}
			}
		});
		return Count;
	}

	// 맵 두 개 (Maps/A, Maps/B) + 스크립트 + 플레이어 프리팹을 임시 Content에 쓴다
	std::filesystem::path WriteTravelContent()
	{
		RegisterNetworkTypes();
		const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectESceneTravelTests";
		std::filesystem::create_directories(Content / L"Maps");
		std::filesystem::create_directories(Content / L"Scripts");
		{
			std::ofstream File(Content / L"Scripts/Traveler.lua", std::ios::binary | std::ios::trunc);
			File << R"(
local T = { Properties = { Target = "", Seen = -1, Scene = "" } }
function T:OnStart()
	self.Properties.Seen  = Game.GetPersistent("Count", 0)
	self.Properties.Scene = Game.GetCurrentScene()
	Game.SetPersistent("Count", self.Properties.Seen + 1)
	Game.SetPersistent("Where", Vector3(1, 2, 3))
end
function T:OnUpdate(dt)
	if self.Properties.Target ~= "" and not self.Sent then
		self.Sent = true
		Game.OpenScene(self.Properties.Target)
	end
end
function T:OnDestroy() Game.SetPersistent("Destroyed_" .. self.entity:GetName(), true) end
return T
)";
		}
		const auto WriteMap = [&](const wchar_t* File, std::initializer_list<const char*> Replicated, const char* ScriptEntity, const char* Target) {
			FScene Map;
			for (const char* Name : Replicated)
			{
				Map.GetRegistry().Emplace<FReplicatedComponent>(Map.CreateEntity(Name));
			}
			Map.CreateEntity("Static");
			const FEntity Start = Map.CreateEntity(FNetPlayerSpawner::PlayerStartName);
			Map.GetTransform(Start).Position = FVector3(100.0f, 0.0f, 0.0f);
			if (ScriptEntity != nullptr)
			{
				FScriptComponent& Script = Map.GetRegistry().Emplace<FScriptComponent>(Map.CreateEntity(ScriptEntity));
				Script.ScriptAsset       = "Scripts/Traveler.lua";
				Script.PropertyOverrides = std::string(R"({"Target":")") + Target + "\"}";
			}
			E_EXPECT_TRUE(FSceneSerializer::SaveToFile(Map, Content / File));
		};
		WriteMap(L"Maps/A.escene", { "Crate", "Barrel" }, "InA", "Maps/B.escene");
		WriteMap(L"Maps/B.escene", { "Box" }, "InB", "");
		WriteMap(L"Maps/NA.escene", { "Crate", "Barrel" }, nullptr, ""); // 네트워크 시험용 (스크립트 없음)

		// 플레이어 프리팹 (복제 루트)
		std::filesystem::remove(Content / L"Pawn.eprefab");
		FPrefabLibrary::Get().SetContentDirectory(Content);
		FScene        Authoring;
		const FEntity Pawn = Authoring.CreateEntity("Pawn");
		Authoring.GetRegistry().Emplace<FReplicatedComponent>(Pawn);
		FPrefabLibrary::Get().CreatePrefab(Authoring, Pawn, Content / L"Pawn.eprefab");
		FPrefabLibrary::Get().Invalidate();
		return Content;
	}

	struct FTravelModule final : IGameModule
	{
		int32 BeginCount = 0, EndCount = 0, Updates = 0;
		bool  bRequested = false, bAccepted = false, bRejected = true;
		void  OnBeginPlay(FScene&) override { ++BeginCount; }
		void  OnEndPlay(FScene&) override { ++EndCount; }
		void  OnUpdate(FScene&, float) override
		{
			if (++Updates == 2 && !bRequested)
			{
				bRequested = true;
				bRejected  = GetNet()->OpenScene("Maps/Missing.escene"); // 없는 파일은 거절
				bAccepted  = GetNet()->OpenScene("Maps/A.escene");
			}
		}
	};
} // namespace

// Standalone: Lua Game.OpenScene → 프레임 끝 전환 → 이전 스크립트 OnDestroy, 새 씬 시작, Game.SetPersistent 값 유지, 현재 씬 이름
E_TEST(SceneTravel_StandaloneLuaOpenScene)
{
	const std::filesystem::path Content = WriteTravelContent();
	FScene                      Scene;
	E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Scene, Content / L"Maps/A.escene"));
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.SetCurrentSceneAsset("Maps/A.escene");
	World.BeginPlay(Scene);
	World.TickGameplay(1.0f / 60.0f, nullptr); // OnStart + OnUpdate → OpenScene 요청
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(FindNamed(Scene, "InA"), "Seen").Number, 0.0, 1.0e-9);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(FindNamed(Scene, "InA"), "Scene").String == "Maps/A.escene");
	E_EXPECT_TRUE(FindNamed(Scene, "Crate").IsValid()); // 갱신 도중에는 바꾸지 않는다

	const std::optional<std::string> Next = FGameWorldTravel::ConsumePending(World, nullptr);
	E_EXPECT_TRUE(Next.has_value() && *Next == "Maps/B.escene");
	E_EXPECT_FALSE(FGameWorldTravel::ConsumePending(World, nullptr).has_value()); // 한 번만
	FSceneTravelTargets Targets;
	Targets.World            = &World;
	Targets.Scene            = &Scene;
	Targets.ContentDirectory = Content;
	E_EXPECT_TRUE(FGameWorldTravel::Travel(Targets, *Next));
	E_EXPECT_TRUE(World.IsPlaying());
	E_EXPECT_TRUE(World.GetCurrentSceneAsset() == "Maps/B.escene");
	E_EXPECT_FALSE(FindNamed(Scene, "Crate").IsValid());
	E_EXPECT_TRUE(FindNamed(Scene, "Box").IsValid());
	E_EXPECT_TRUE(Scripts.GetPersistentValues().contains("Destroyed_InA")); // 이전 씬 OnDestroy가 돌았다

	World.TickGameplay(1.0f / 60.0f, nullptr);
	const FEntity InB = FindNamed(Scene, "InB");
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(InB, "Seen").Number, 1.0, 1.0e-9); // 새 Lua 상태에도 값이 남는다
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(InB, "Scene").String == "Maps/B.escene");
	E_EXPECT_TRUE(Scripts.RunString("local V = Game.GetPersistent('Where'); assert(V.Y == 2)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(Game.GetPersistent('Nothing', 7) == 7)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// 없는 씬은 스크립트 오류, 테이블 값은 저장 못 함
	E_EXPECT_FALSE(Scripts.RunString("Game.OpenScene('Maps/Missing.escene')"));
	E_EXPECT_FALSE(Scripts.RunString("Game.SetPersistent('T', {})"));
	E_EXPECT_FALSE(FGameWorldTravel::ConsumePending(World, nullptr).has_value());
	World.EndPlay();
	FPrefabLibrary::Get().SetContentDirectory(std::filesystem::path());
}

// 게임 모듈 C++ API: IGameNet::OpenScene (없는 파일 거절), 전환 시 OnEndPlay → OnBeginPlay
E_TEST(SceneTravel_GameModuleOpenScene)
{
	const std::filesystem::path Content = WriteTravelContent();
	FScene                      Scene;
	E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Scene, Content / L"Maps/B.escene"));
	FTravelModule   Module;
	FGameModuleHost Host;
	Host.Attach(Module, "TravelModule");
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(1.0f / 60.0f, nullptr);
	World.TickGameplay(1.0f / 60.0f, nullptr);
	E_EXPECT_FALSE(Module.bRejected);
	E_EXPECT_TRUE(Module.bAccepted);
	const std::optional<std::string> Next = FGameWorldTravel::ConsumePending(World, nullptr);
	E_EXPECT_TRUE(Next.has_value() && *Next == "Maps/A.escene");
	FSceneTravelTargets Targets;
	Targets.World            = &World;
	Targets.Scene            = &Scene;
	Targets.ContentDirectory = Content;
	FGameWorldTravel::Travel(Targets, *Next);
	E_EXPECT_EQ(Module.EndCount, 1);
	E_EXPECT_EQ(Module.BeginCount, 2);
	E_EXPECT_TRUE(Module.GetNet() != nullptr);
	E_EXPECT_TRUE(FindNamed(Scene, "Crate").IsValid());
	World.EndPlay();
	Host.Unload();
	FPrefabLibrary::Get().SetContentDirectory(std::filesystem::path());
}

namespace
{
	// 루프백 전용 서버 + 클라이언트들 (앱처럼 연결하고, 맵 전환은 앱과 같은 FGameWorldTravel 경로)
	struct FTravelServer
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationServer Replication;
		FNetPlayerSpawner  Players;
		std::filesystem::path Content;

		void Start(const std::shared_ptr<FLoopbackHub>& Hub, const std::filesystem::path& InContent)
		{
			Content = InContent;
			E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Scene, Content / L"Maps/NA.escene"));
			World.Init({ &Scripts, nullptr, nullptr, nullptr, Content, &Net });
			World.SetCurrentSceneAsset("Maps/NA.escene");
			FNetSessionInfo Session;
			Session.ProjectName = "Test";
			Session.SceneAsset  = "Maps/NA.escene";
			Net.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
			Replication.Begin(Scene, Net);
			Players.Begin(Scene, "Pawn.eprefab");
			Net.OnPlayerJoined = [this](const FNetDriver::FRemotePlayer& Player) {
				const FEntity Pawn = Players.SpawnPlayer(Player.PlayerId);
				Replication.OnPlayerJoined(Player.Connection);
				World.OnPlayerJoined(Player.PlayerId, Pawn);
			};
			Net.OnPlayerLeft = [this](const FNetDriver::FRemotePlayer& Player, const std::string&) {
				World.OnPlayerLeft(Player.PlayerId);
				Players.DespawnPlayer(Player.PlayerId);
			};
			Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) { World.HandleNetMessage(Connection, Message); };
			World.BeginPlay(Scene, ENetMode::DedicatedServer);
		}

		void Tick(float Step)
		{
			Net.Update(Step);
			World.TickGameplay(Step, nullptr);
			Replication.Tick(Step);
			if (const std::optional<std::string> Next = FGameWorldTravel::ConsumePending(World, &Net))
			{
				FSceneTravelTargets Targets;
				Targets.World             = &World;
				Targets.Scene             = &Scene;
				Targets.Net               = &Net;
				Targets.ReplicationServer = &Replication;
				Targets.Players           = &Players;
				Targets.ContentDirectory  = Content;
				Targets.PlayerPrefab      = "Pawn.eprefab";
				FGameWorldTravel::Travel(Targets, *Next);
			}
		}
	};

	struct FTravelClient
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationClient Replication;
		std::filesystem::path Content;
		int32              Travels = 0;

		void Start(const std::shared_ptr<FLoopbackHub>& Hub, const std::filesystem::path& InContent, const std::string& SceneAsset)
		{
			Content = InContent;
			E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Scene, Content / FStringConv::ToWide(SceneAsset)));
			World.Init({ &Scripts, nullptr, nullptr, nullptr, Content, &Net });
			World.SetCurrentSceneAsset(SceneAsset);
			Replication.Begin(Scene);
			Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) {
				if (!Replication.HandleMessage(Message))
				{
					World.HandleNetMessage(Connection, Message);
				}
			};
			World.BeginPlay(Scene, ENetMode::Client);
			FNetSessionInfo Session;
			Session.ProjectName = "Test";
			Session.SceneAsset  = SceneAsset;
			Net.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Session);
		}


		void Tick(float Step)
		{
			Net.Update(Step);
			Replication.Update(Step);
			World.TickGameplay(Step, nullptr);
			if (const std::optional<std::string> Next = FGameWorldTravel::ConsumePending(World, &Net))
			{
				FSceneTravelTargets Targets;
				Targets.World             = &World;
				Targets.Scene             = &Scene;
				Targets.Net               = &Net;
				Targets.ReplicationClient = &Replication;
				Targets.ContentDirectory  = Content;
				FGameWorldTravel::Travel(Targets, *Next);
				++Travels;
			}
		}
	};
} // namespace

// 서버 이동 (언리얼 ServerTravel): 서버가 맵을 바꾸면 접속한 클라이언트가 따라오고, 새 씬에서 플레이어 폰이 다시 생기고,
// 정적 NetId가 서버와 같다. 이동 뒤 입장은 새 씬으로만 된다. 클라이언트의 Game.OpenScene은 거절
E_TEST(SceneTravel_ServerTravelsConnectedClients)
{
	const std::filesystem::path Content = WriteTravelContent();
	auto                        Hub     = std::make_shared<FLoopbackHub>();
	FTravelServer               Server;
	Server.Start(Hub, Content);
	FTravelClient Client;
	Client.Start(Hub, Content, "Maps/NA.escene");
	const auto Pump = [&](int32 Rounds, std::initializer_list<FTravelClient*> Clients) {
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			constexpr float Step = 1.0f / 30.0f;
			Server.Tick(Step);
			for (FTravelClient* Each : Clients)
			{
				Each->Tick(Step);
			}
		}
	};
	Pump(8, { &Client });
	E_EXPECT_TRUE(Client.Net.GetClientState() == FNetDriver::EClientState::Joined);
	const int32 PlayerId = static_cast<int32>(Client.Net.GetLocalPlayerId());
	int32       Owned    = 0;
	E_EXPECT_EQ(CountPawns(Client.Scene, &Owned, PlayerId), 1);
	E_EXPECT_EQ(Owned, 1);
	E_EXPECT_TRUE(Client.Scripts.RunString("assert(Game.OpenScene('Maps/B.escene') == false)")); // 클라이언트는 따라가기만
	E_EXPECT_FALSE(FGameWorldTravel::ConsumePending(Client.World, &Client.Net).has_value());

	// 서버(C++/게임 모듈 경로)가 B로
	E_EXPECT_TRUE(Server.World.OpenScene("Maps/B.escene"));
	Pump(10, { &Client });
	E_EXPECT_EQ(Client.Travels, 1);
	E_EXPECT_TRUE(Client.World.GetCurrentSceneAsset() == "Maps/B.escene");
	E_EXPECT_TRUE(Server.Net.GetSessionScene() == "Maps/B.escene");
	E_EXPECT_TRUE(Client.Net.GetSessionScene() == "Maps/B.escene");
	E_EXPECT_FALSE(FindNamed(Client.Scene, "Crate").IsValid());
	const FEntity ServerBox = FindNamed(Server.Scene, "Box");
	const FEntity ClientBox = FindNamed(Client.Scene, "Box");
	E_EXPECT_TRUE(ServerBox.IsValid() && ClientBox.IsValid());
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, ClientBox), NetReplication::GetNetId(Server.Scene, ServerBox));
	Owned = 0;
	E_EXPECT_EQ(CountPawns(Client.Scene, &Owned, PlayerId), 1); // 새 씬에서 다시 생성된 내 폰 하나만
	E_EXPECT_EQ(Owned, 1);
	E_EXPECT_EQ(CountPawns(Server.Scene), 1);
	E_EXPECT_TRUE(Server.Players.FindPawn(static_cast<uint32>(PlayerId)).IsValid());
	E_EXPECT_EQUALS(Server.Scene.GetTransform(Server.Players.FindPawn(static_cast<uint32>(PlayerId))).Position, FVector3(100.0f, 0.0f, 0.0f), 1.0e-3f);

	// 이동 뒤 서버 변경이 새 씬 엔티티에 복제된다
	Server.Scene.GetTransform(ServerBox).Position = FVector3(0.0f, 300.0f, 0.0f);
	Pump(20, { &Client });
	E_EXPECT_EQUALS(Client.Scene.GetTransform(ClientBox).Position, FVector3(0.0f, 300.0f, 0.0f), 1.0e-2f);

	// 늦은 입장: 이전 씬으로는 거부, 새 씬으로는 입장해 폰 두 개를 본다
	FTravelClient Stale;
	Stale.Start(Hub, Content, "Maps/NA.escene");
	Pump(6, { &Client, &Stale });
	E_EXPECT_TRUE(Stale.Net.GetClientState() == FNetDriver::EClientState::Failed);
	FTravelClient Late;
	Late.Start(Hub, Content, "Maps/B.escene");
	Pump(8, { &Client, &Late });
	E_EXPECT_TRUE(Late.Net.GetClientState() == FNetDriver::EClientState::Joined);
	E_EXPECT_EQ(CountPawns(Late.Scene), 2);
	E_EXPECT_EQ(CountPawns(Client.Scene), 2);

	// 다시 A로 (두 클라이언트 모두 따라온다)
	E_EXPECT_TRUE(Server.World.OpenScene("Maps/NA.escene"));
	Pump(10, { &Client, &Late });
	E_EXPECT_EQ(Client.Travels, 2);
	E_EXPECT_EQ(Late.Travels, 1);
	E_EXPECT_TRUE(FindNamed(Late.Scene, "Crate").IsValid() && !FindNamed(Late.Scene, "Box").IsValid());
	E_EXPECT_EQ(CountPawns(Server.Scene), 2);
	E_EXPECT_EQ(CountPawns(Client.Scene), 2);
	E_EXPECT_EQ(CountPawns(Late.Scene), 2);
	E_EXPECT_EQ(NetReplication::GetNetId(Late.Scene, FindNamed(Late.Scene, "Barrel")), NetReplication::GetNetId(Server.Scene, FindNamed(Server.Scene, "Barrel")));
	E_EXPECT_EQ(Server.Scripts.GetErrorCount() + Client.Scripts.GetErrorCount() + Late.Scripts.GetErrorCount(), 0u);

	Client.World.EndPlay();
	Late.World.EndPlay();
	Stale.World.EndPlay();
	Server.World.EndPlay();
	FPrefabLibrary::Get().SetContentDirectory(std::filesystem::path());
}

// 에디터 플레이: 맵 전환은 플레이 씬 내용만 바꾸고 (Context.Scene 포인터 유지), 정지하면 편집 씬이 그대로 돌아오고 SetPersistent 값은 비워진다
E_TEST(SceneTravel_PlayModeTravelThenStopRestoresEditScene)
{
	const std::filesystem::path Content = WriteTravelContent();
	FScene                      EditScene;
	E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(EditScene, Content / L"Maps/A.escene"));
	const std::string Before    = FSceneSerializer::ToJsonString(EditScene);
	const FEntity     EditCrate = FindNamed(EditScene, "Crate");

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	FPlayMode PlayMode;
	PlayMode.Init(EditScene, World);
	FEditorContext Context;
	Context.Scene            = &EditScene;
	Context.ContentDirectory = Content;
	Context.Select(EditCrate);
	PlayMode.Play(Context);
	E_EXPECT_TRUE(PlayMode.Tick(Context, 1.0f / 60.0f, nullptr)); // InA → OpenScene(B)
	const std::optional<std::string> Next = FGameWorldTravel::ConsumePending(World, nullptr);
	E_EXPECT_TRUE(Next.has_value());
	PlayMode.Travel(Context, {}, Next.value_or(std::string()));
	E_EXPECT_TRUE(Context.Scene == &PlayMode.GetPlayScene());
	E_EXPECT_TRUE(FindNamed(PlayMode.GetPlayScene(), "Box").IsValid());
	E_EXPECT_FALSE(Context.SelectedEntity.IsValid());
	E_EXPECT_TRUE(PlayMode.Tick(Context, 1.0f / 60.0f, nullptr));
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(FindNamed(PlayMode.GetPlayScene(), "InB"), "Seen").Number, 1.0, 1.0e-9);

	PlayMode.Stop(Context);
	E_EXPECT_TRUE(Context.Scene == &EditScene);
	E_EXPECT_TRUE(Context.SelectedEntity == EditCrate);
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(EditScene) == Before);
	E_EXPECT_TRUE(Scripts.GetPersistentValues().empty());
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	FPrefabLibrary::Get().SetContentDirectory(std::filesystem::path());
}

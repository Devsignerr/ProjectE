// 서브 씬 스트리밍 (Phase 31-2): Lua Scene.LoadSubScene/UnloadSubScene(백그라운드 파싱), 스트리밍 볼륨, 서버가 불러온 서브 씬을 클라이언트가 따라 붙이기
#include "Core/Log.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/SubScene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"
#include "World/GameWorldTravel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

E_DECLARE_LOG_CATEGORY(LogSubSceneTest)
E_DEFINE_LOG_CATEGORY(LogSubSceneTest, Log)

namespace
{
	FEntity FindByName(FScene& Scene, const std::string& Name)
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

	// Maps/Main.escene (리스너 스크립트), Sub/Room.escene (Chair, 복제 Door, 스크립트 Lamp), Sub/Big.escene (측정용 엔티티 1000개)
	std::filesystem::path WriteSubSceneContent()
	{
		RegisterNetworkTypes();
		const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectESubSceneTests";
		std::filesystem::create_directories(Content / L"Maps");
		std::filesystem::create_directories(Content / L"Sub");
		std::filesystem::create_directories(Content / L"Scripts");
		{
			std::ofstream File(Content / L"Scripts/Listener.lua", std::ios::binary | std::ios::trunc);
			File << R"(
local T = { Properties = { Loaded = "" } }
function T:OnSubSceneLoaded(path) self.Properties.Loaded = path end
return T
)";
		}
		{
			std::ofstream File(Content / L"Scripts/Lamp.lua", std::ios::binary | std::ios::trunc);
			File << R"(
local T = {}
function T:OnStart() Game.SetPersistent("LampStarted", (Game.GetPersistent("LampStarted", 0)) + 1) end
function T:OnDestroy() Game.SetPersistent("LampDestroyed", true) end
return T
)";
		}
		{
			FScene Main;
			Main.GetRegistry().Emplace<FScriptComponent>(Main.CreateEntity("Listener")).ScriptAsset = "Scripts/Listener.lua";
			Main.GetRegistry().Emplace<FReplicatedComponent>(Main.CreateEntity("MainCrate"));
			E_EXPECT_TRUE(FSceneSerializer::SaveToFile(Main, Content / L"Maps/Main.escene"));
		}
		{
			FScene        Room;
			const FEntity Chair = Room.CreateEntity("Chair");
			Room.GetTransform(Chair).Position = FVector3(10.0f, 20.0f, 0.0f);
			const FEntity Door = Room.CreateEntity("Door");
			Room.GetRegistry().Emplace<FReplicatedComponent>(Door);
			const FEntity Knob = Room.CreateEntity("Knob");
			Room.SetParent(Knob, Door);
			Room.GetRegistry().Emplace<FReplicatedComponent>(Knob);
			Room.GetRegistry().Emplace<FScriptComponent>(Room.CreateEntity("Lamp")).ScriptAsset = "Scripts/Lamp.lua";
			E_EXPECT_TRUE(FSceneSerializer::SaveToFile(Room, Content / L"Sub/Room.escene"));
		}
		if (!std::filesystem::exists(Content / L"Sub/Big.escene"))
		{
			FScene Big;
			for (int32 Index = 0; Index < 1000; ++Index)
			{
				const FEntity Entity = Big.CreateEntity("Block");
				Big.GetTransform(Entity).Position = FVector3(static_cast<float>(Index % 40) * 100.0f, static_cast<float>(Index / 40) * 100.0f, 0.0f);
				FStaticMeshComponent& Mesh = Big.GetRegistry().Emplace<FStaticMeshComponent>(Entity);
				Mesh.MeshAsset             = "primitive:cube";
			}
			E_EXPECT_TRUE(FSceneSerializer::SaveToFile(Big, Content / L"Sub/Big.escene"));
		}
		return Content;
	}

	// 백그라운드 파싱이 끝나 붙을 때까지 틱 (최대 3초)
	bool TickUntilLoaded(FGameWorld& World, const std::string& Asset)
	{
		for (int32 Attempt = 0; Attempt < 300 && !World.IsSubSceneLoaded(Asset); ++Attempt)
		{
			World.TickGameplay(1.0f / 60.0f, nullptr);
			if (!World.IsSubSceneLoaded(Asset))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		return World.IsSubSceneLoaded(Asset);
	}
} // namespace

// Lua: 불러오기(백그라운드 파싱 → 다음 틱에 루트 아래로, 위치 오프셋) → OnSubSceneLoaded, 서브 씬 스크립트 시작, 저장에서 제외 →
// 내리기(OnDestroy 뒤 루트째 제거). 없는 파일은 스크립트 오류
E_TEST(SubScene_LuaLoadUnloadBackgroundParse)
{
	const std::filesystem::path Content = WriteSubSceneContent();
	FScene                      Scene;
	E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Scene, Content / L"Maps/Main.escene"));
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(1.0f / 60.0f, nullptr);
	const size_t EntitiesBefore = Scene.GetRegistry().GetAliveCount();

	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.LoadSubScene('Sub/Room.escene', Vector3(1000, 0, 0)) == true)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.LoadSubScene(Asset('Sub/Room.escene', '.escene')) == true)")); // 이미 불러오는 중 → true
	E_EXPECT_FALSE(World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_TRUE(TickUntilLoaded(World, "Sub/Room.escene"));
	World.TickGameplay(1.0f / 60.0f, nullptr);

	const FEntity Chair = FindByName(Scene, "Chair");
	const FEntity Root  = World.GetSubSceneRoot("Sub/Room.escene");
	E_EXPECT_TRUE(Chair.IsValid() && Root.IsValid());
	E_EXPECT_TRUE(Scene.GetParent(Chair) == Root);
	E_EXPECT_TRUE(Scene.GetRegistry().Has<FTransientComponent>(Root));
	E_EXPECT_EQUALS(Scene.GetTransform(Chair).GetWorldPosition(), FVector3(1010.0f, 20.0f, 0.0f), 1.0e-3f);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(FindByName(Scene, "Listener"), "Loaded").String == "Sub/Room.escene");
	E_EXPECT_NEAR(Scripts.GetPersistentValues()["LampStarted"].Number, 1.0, 1.0e-9);
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.IsSubSceneLoaded('Sub/Room.escene')); assert(Scene.GetSubSceneRoot('Sub/Room.escene'):GetName() == 'SubScene: Sub/Room.escene')"));
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(Scene).find("Chair") == std::string::npos); // 서브 씬은 메인 씬 저장에 섞이지 않는다
	E_EXPECT_TRUE(World.GetSubSceneStats().LastParseMs > 0.0f);

	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.UnloadSubScene('Sub/Room.escene') == true)"));
	World.TickGameplay(1.0f / 60.0f, nullptr);
	E_EXPECT_FALSE(FindByName(Scene, "Chair").IsValid());
	E_EXPECT_FALSE(Scene.GetRegistry().IsValid(Root));
	E_EXPECT_EQ(Scene.GetRegistry().GetAliveCount(), EntitiesBefore);
	E_EXPECT_TRUE(Scripts.GetPersistentValues().contains("LampDestroyed"));
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.UnloadSubScene('Sub/Room.escene') == false)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_FALSE(Scripts.RunString("Scene.LoadSubScene('Sub/Missing.escene')"));

	// 불러오는 중에 내리면 결과를 버린다
	E_EXPECT_TRUE(World.RequestLoadSubScene("Sub/Room.escene", FVector3::ZeroVector));
	E_EXPECT_TRUE(World.UnloadSubScene("Sub/Room.escene"));
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
	}
	E_EXPECT_FALSE(FindByName(Scene, "Chair").IsValid());

	// 측정: 엔티티 1000개 서브 씬 (파싱 = 백그라운드, 붙이기 = 메인 스레드 멈칫함)
	E_EXPECT_TRUE(World.RequestLoadSubScene("Sub/Big.escene", FVector3::ZeroVector));
	E_EXPECT_TRUE(TickUntilLoaded(World, "Sub/Big.escene"));
	E_LOG(LogSubSceneTest, Display, "서브 씬 측정 (엔티티 1000개): 파싱 {:.2f}ms (백그라운드), 붙이기 {:.2f}ms (메인)", World.GetSubSceneStats().LastParseMs,
	      World.GetSubSceneStats().LastAttachMs);
	World.EndPlay();
}

// 스트리밍 볼륨: 기준(StreamingSourceComponent)이 상자 안이면 불러오고, 상자 + 여유 밖이면 내린다. 스크립트가 불러온 것은 내리지 않는다
E_TEST(SubScene_VolumeStreamsByDistance)
{
	const std::filesystem::path Content = WriteSubSceneContent();
	FScene                      Scene;
	const FEntity               Volume = Scene.CreateEntity("Volume");
	FSubSceneVolumeComponent&   Box    = Scene.GetRegistry().Emplace<FSubSceneVolumeComponent>(Volume);
	Box.SubScene                       = "Sub/Room.escene";
	Box.HalfExtents                    = FVector3(500.0f, 500.0f, 500.0f);
	Box.UnloadMargin                   = 200.0f;
	const FEntity Walker               = Scene.CreateEntity("Walker");
	Scene.GetRegistry().Emplace<FStreamingSourceComponent>(Walker);
	Scene.GetTransform(Walker).Position = FVector3(2000.0f, 0.0f, 0.0f);
	Scene.UpdateTransforms();

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.bAsyncSubSceneLoad = false; // 같은 틱에 붙인다 (판정만 시험)
	World.BeginPlay(Scene);
	const auto TickAt = [&](float X) {
		Scene.GetTransform(Walker).Position.X = X;
		Scene.UpdateTransforms();
		World.TickGameplay(1.0f / 60.0f, nullptr);
		World.TickGameplay(1.0f / 60.0f, nullptr); // 지연 파괴 적용
	};
	TickAt(2000.0f);
	E_EXPECT_FALSE(World.IsSubSceneLoaded("Sub/Room.escene"));
	TickAt(450.0f);
	E_EXPECT_TRUE(World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_TRUE(FindByName(Scene, "Chair").IsValid());
	TickAt(650.0f); // 상자 밖이지만 여유 안
	E_EXPECT_TRUE(World.IsSubSceneLoaded("Sub/Room.escene"));
	TickAt(800.0f);
	E_EXPECT_FALSE(World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_FALSE(FindByName(Scene, "Chair").IsValid());
	TickAt(0.0f);
	E_EXPECT_TRUE(World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_EQ(World.GetSubSceneStats().Loads, 2u);

	// 스크립트가 요청한 서브 씬은 볼륨이 내리지 않는다
	E_EXPECT_TRUE(World.RequestLoadSubScene("Sub/Room.escene", FVector3::ZeroVector));
	TickAt(5000.0f);
	E_EXPECT_TRUE(World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

namespace
{
	struct FSubServer
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationServer Replication;
	};
	struct FSubClient
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationClient Replication;

		void Start(const std::shared_ptr<FLoopbackHub>& Hub, const std::filesystem::path& Content)
		{
			E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Scene, Content / L"Maps/Main.escene"));
			World.Init({ &Scripts, nullptr, nullptr, nullptr, Content, &Net });
			Replication.Begin(Scene);
			World.SetReplicationClient(&Replication);
			Net.OnGameMessage = [this](FNetConnectionId Connection, const std::vector<uint8>& Message) {
				if (!Replication.HandleMessage(Message))
				{
					World.HandleNetMessage(Connection, Message);
				}
			};
			World.BeginPlay(Scene, ENetMode::Client);
			FNetSessionInfo Session;
			Session.SceneAsset = "Maps/Main.escene";
			Net.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Session);
		}
		void Tick(float Step)
		{
			Net.Update(Step);
			Replication.Update(Step);
			World.TickGameplay(Step, nullptr);
		}
	};
} // namespace

// 멀티플레이: 서버가 불러온 서브 씬을 클라이언트도 같은 파일로 붙이고(같은 NetId 구간), 이후 상태가 복제되고, 늦은 입장자도 받고,
// 서버가 내리면 클라이언트에서도 사라진다. 클라이언트의 Scene.LoadSubScene은 거절
E_TEST(SubScene_ClientFollowsServerSubScenes)
{
	const std::filesystem::path Content = WriteSubSceneContent();
	auto                        Hub     = std::make_shared<FLoopbackHub>();
	FSubServer                  Server;
	E_EXPECT_TRUE(FSceneSerializer::LoadFromFile(Server.Scene, Content / L"Maps/Main.escene"));
	Server.World.Init({ &Server.Scripts, nullptr, nullptr, nullptr, Content, &Server.Net });
	FNetSessionInfo Session;
	Session.SceneAsset = "Maps/Main.escene";
	Server.Net.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
	Server.Replication.Begin(Server.Scene, Server.Net);
	Server.World.SetReplicationServer(&Server.Replication);
	Server.Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) { Server.Replication.OnPlayerJoined(Player.Connection); };
	Server.Net.OnGameMessage  = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Server.World.HandleNetMessage(Connection, Message); };
	Server.World.BeginPlay(Server.Scene, ENetMode::DedicatedServer);

	FSubClient Client;
	Client.Start(Hub, Content);
	const auto Pump = [&](int32 Rounds, std::initializer_list<FSubClient*> Clients) {
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			constexpr float Step = 1.0f / 30.0f;
			Server.Net.Update(Step);
			Server.World.TickGameplay(Step, nullptr);
			Server.Replication.Tick(Step);
			for (FSubClient* Each : Clients)
			{
				Each->Tick(Step);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2)); // 서버 백그라운드 파싱
		}
	};
	Pump(6, { &Client });
	E_EXPECT_TRUE(Client.Net.GetClientState() == FNetDriver::EClientState::Joined);
	E_EXPECT_TRUE(Client.Scripts.RunString("assert(Scene.LoadSubScene('Sub/Room.escene') == false)"));

	E_EXPECT_TRUE(Server.World.LoadSubScene("Sub/Room.escene", FVector3(0.0f, 500.0f, 0.0f))); // 게임 모듈 경로
	for (int32 Round = 0; Round < 100 && !Server.World.IsSubSceneLoaded("Sub/Room.escene"); ++Round)
	{
		Pump(1, { &Client });
	}
	Pump(4, { &Client });
	const FEntity ServerDoor = FindByName(Server.Scene, "Door");
	const FEntity ClientDoor = FindByName(Client.Scene, "Door");
	E_EXPECT_TRUE(ServerDoor.IsValid() && ClientDoor.IsValid());
	E_EXPECT_TRUE(Client.World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, ClientDoor), NetReplication::GetNetId(Server.Scene, ServerDoor));
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, FindByName(Client.Scene, "Knob")), NetReplication::GetNetId(Server.Scene, FindByName(Server.Scene, "Knob")));
	E_EXPECT_TRUE(NetReplication::GetNetId(Server.Scene, ServerDoor) >= SubSceneNetIdBase);
	E_EXPECT_EQUALS(Client.Scene.GetTransform(FindByName(Client.Scene, "Chair")).GetWorldPosition(), FVector3(10.0f, 520.0f, 0.0f), 1.0e-3f);
	E_EXPECT_TRUE(Client.Scripts.GetInstanceProperty(FindByName(Client.Scene, "Listener"), "Loaded").String.empty()); // Listener는 ServerOnly
	E_EXPECT_TRUE(Server.Scripts.GetInstanceProperty(FindByName(Server.Scene, "Listener"), "Loaded").String == "Sub/Room.escene");

	// 서브 씬 엔티티 상태 복제
	Server.Scene.GetRegistry().Get<FReplicatedComponent>(ServerDoor).OwnerPlayerId = 3;
	Pump(4, { &Client });
	E_EXPECT_EQ(Client.Scene.GetRegistry().Get<FReplicatedComponent>(ClientDoor).OwnerPlayerId, 3);

	// 늦은 입장자: 서브 씬부터 받는다
	FSubClient Late;
	Late.Start(Hub, Content);
	Pump(8, { &Client, &Late });
	const FEntity LateDoor = FindByName(Late.Scene, "Door");
	E_EXPECT_TRUE(LateDoor.IsValid());
	E_EXPECT_EQ(NetReplication::GetNetId(Late.Scene, LateDoor), NetReplication::GetNetId(Server.Scene, ServerDoor));
	E_EXPECT_EQ(Late.Scene.GetRegistry().Get<FReplicatedComponent>(LateDoor).OwnerPlayerId, 3);

	// 서버가 내리면 모두에서 사라진다
	E_EXPECT_TRUE(Server.World.UnloadSubScene("Sub/Room.escene"));
	Pump(6, { &Client, &Late });
	E_EXPECT_FALSE(FindByName(Server.Scene, "Door").IsValid());
	E_EXPECT_FALSE(FindByName(Client.Scene, "Door").IsValid());
	E_EXPECT_FALSE(FindByName(Late.Scene, "Chair").IsValid());
	E_EXPECT_FALSE(Client.World.IsSubSceneLoaded("Sub/Room.escene"));
	E_EXPECT_TRUE(FindByName(Client.Scene, "MainCrate").IsValid()); // 메인 씬은 그대로
	E_EXPECT_EQ(Server.Scripts.GetErrorCount() + Client.Scripts.GetErrorCount() + Late.Scripts.GetErrorCount(), 0u);

	Client.World.EndPlay();
	Late.World.EndPlay();
	Server.World.EndPlay();
}

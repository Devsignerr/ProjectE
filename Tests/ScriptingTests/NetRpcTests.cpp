#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	// RPC 흐름 기록 스크립트 (Both): 클라이언트 → Server_Fire → 서버가 Client_Ack(소유자) + Multicast_Boom(모두)
	std::filesystem::path WriteRpcScript()
	{
		const std::filesystem::path Directory = std::filesystem::temp_directory_path() / L"ProjectENetRpcTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/Rpc.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Fired = 0, Acked = 0, Boomed = "", Target = "" } }
function T:Server_Fire(n, where, target)
	self.Properties.Fired  = n
	self.Properties.Target = target and target:GetName() or "없음"
	self.entity:CallClient("Ack", n + 1)
	self.entity:CallMulticast("Boom", "펑" .. tostring(where.X))
end
function T:Client_Ack(n) self.Properties.Acked = n end
function T:Multicast_Boom(s) self.Properties.Boomed = s end
return T
)";
		return Directory;
	}

	void BuildLevel(FScene& Scene)
	{
		for (const char* Name : { "Pawn", "Other" })
		{
			const FEntity     Entity = Scene.CreateEntity(Name);
			FScriptComponent& Script = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
			Script.ScriptAsset       = "Scripts/Rpc.lua";
			Script.ExecutionLocation = static_cast<int32>(EScriptExecution::Both);
			Scene.GetRegistry().Emplace<FReplicatedComponent>(Entity);
		}
		Scene.UpdateTransforms();
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

	double NumberProperty(FScriptSystem& Scripts, FEntity Entity, const std::string& Name)
	{
		return Scripts.GetInstanceProperty(Entity, Name).Number;
	}
} // namespace

// Standalone(네트워크 없음): 모든 RPC가 바로 로컬에서 불린다
E_TEST(NetRpc_StandaloneCallsLocally)
{
	RegisterNetworkTypes();
	const std::filesystem::path Content = WriteRpcScript();
	FScene                      Scene;
	BuildLevel(Scene);
	Scene.GetRegistry().Get<FReplicatedComponent>(Find(Scene, "Pawn")).OwnerPlayerId = 0; // 로컬 플레이어
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(1.0f / 60.0f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Pawn'):CallServer('Fire', 41, Vector3(7, 0, 0), Scene.Find('Other'))"));
	const FEntity Pawn = Find(Scene, "Pawn");
	E_EXPECT_NEAR(NumberProperty(Scripts, Pawn, "Fired"), 41.0, 1.0e-9);
	E_EXPECT_NEAR(NumberProperty(Scripts, Pawn, "Acked"), 42.0, 1.0e-9);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Pawn, "Boomed").String == "펑7.0");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Pawn, "Target").String == "Other");
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 서버(전용) + 클라이언트 (루프백): 소유자의 Server RPC만 실행되고, Client RPC는 소유자에게, Multicast는 모두에게
E_TEST(NetRpc_RoutesBetweenServerAndClient)
{
	RegisterNetworkTypes();
	const std::filesystem::path Content = WriteRpcScript();
	auto                        Hub     = std::make_shared<FLoopbackHub>();
	FNetSessionInfo             Session;
	Session.ProjectName = "Test";

	FScene             ServerScene;
	FScriptSystem      ServerScripts;
	FNetDriver         ServerNet;
	FGameWorld         ServerWorld;
	FReplicationServer ServerReplication;
	BuildLevel(ServerScene);
	ServerWorld.Init({ &ServerScripts, nullptr, nullptr, nullptr, Content, &ServerNet });
	ServerNet.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
	ServerReplication.Begin(ServerScene, ServerNet);
	ServerNet.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
		ServerScene.GetRegistry().Get<FReplicatedComponent>(Find(ServerScene, "Pawn")).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
		ServerReplication.OnPlayerJoined(Player.Connection);
	};
	ServerNet.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { ServerWorld.HandleNetMessage(Connection, Message); };
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
			constexpr float Step = 1.0f / 30.0f;
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
	E_EXPECT_EQ(ClientScene.GetRegistry().Get<FReplicatedComponent>(Find(ClientScene, "Pawn")).OwnerPlayerId,
	            static_cast<int32>(ClientNet.GetLocalPlayerId())); // 소유권 복제됨

	// 클라이언트 → 서버 (자기 폰) → 서버가 소유 클라이언트에게 Ack, 모두에게 Boom
	E_EXPECT_TRUE(ClientScripts.RunString("Scene.Find('Pawn'):CallServer('Fire', 41, Vector3(7, 0, 0), Scene.Find('Other'))"));
	Pump(4);
	const FEntity ServerPawn = Find(ServerScene, "Pawn");
	const FEntity ClientPawn = Find(ClientScene, "Pawn");
	E_EXPECT_NEAR(NumberProperty(ServerScripts, ServerPawn, "Fired"), 41.0, 1.0e-9);
	E_EXPECT_TRUE(ServerScripts.GetInstanceProperty(ServerPawn, "Target").String == "Other"); // 엔티티 인자는 NetId로 옮겨진다
	E_EXPECT_NEAR(NumberProperty(ClientScripts, ClientPawn, "Acked"), 42.0, 1.0e-9);
	E_EXPECT_NEAR(NumberProperty(ServerScripts, ServerPawn, "Acked"), 0.0, 1.0e-9); // Client RPC는 서버에서 돌지 않는다
	E_EXPECT_TRUE(ServerScripts.GetInstanceProperty(ServerPawn, "Boomed").String == "펑7.0");
	E_EXPECT_TRUE(ClientScripts.GetInstanceProperty(ClientPawn, "Boomed").String == "펑7.0");

	// 소유하지 않은 엔티티의 Server RPC는 서버가 거부
	E_EXPECT_TRUE(ClientScripts.RunString("Scene.Find('Other'):CallServer('Fire', 5, Vector3(0, 0, 0))"));
	Pump(4);
	E_EXPECT_NEAR(NumberProperty(ServerScripts, Find(ServerScene, "Other"), "Fired"), 0.0, 1.0e-9);

	// 클라이언트에서 CallClient/CallMulticast는 스크립트 오류
	const uint32 ErrorsBefore = ClientScripts.GetErrorCount();
	E_EXPECT_FALSE(ClientScripts.RunString("Scene.Find('Pawn'):CallMulticast('Boom', 'x')"));
	E_EXPECT_TRUE(ClientScripts.GetErrorCount() > ErrorsBefore);
	E_EXPECT_EQ(ServerScripts.GetErrorCount(), 0u);

	ClientWorld.EndPlay();
	ServerWorld.EndPlay();
}

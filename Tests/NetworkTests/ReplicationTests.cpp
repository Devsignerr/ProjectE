#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <filesystem>

namespace
{
	constexpr float SendStep = 1.0f / 30.0f;

	// 서버와 클라이언트가 같은 "씬 파일"을 로드한 것처럼 같은 순서로 만든다
	void BuildLevel(FScene& Scene)
	{
		Scene.CreateEntity("Sun"); // 복제 안 함
		const FEntity Crate = Scene.CreateEntity("Crate");
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Crate);
		Scene.GetRegistry().Emplace<FStaticMeshComponent>(Crate).MeshAsset = "primitive:cube";
		const FEntity Door = Scene.CreateEntity("Door");
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Door);
		Scene.UpdateTransforms();
	}

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

	FNetSessionInfo Session()
	{
		FNetSessionInfo Info;
		Info.ProjectName = "Test";
		Info.SceneAsset  = "Level";
		return Info;
	}

	// 서버 1 + 클라이언트 N (루프백). 서버는 틱마다 복제를 보내고, 클라이언트는 받은 메시지를 적용한다
	struct FNetFixture
	{
		std::shared_ptr<FLoopbackHub> Hub = std::make_shared<FLoopbackHub>();
		FScene                        ServerScene;
		FNetDriver                    ServerDriver;
		FReplicationServer            Server;

		struct FClient
		{
			FScene             Scene;
			FNetDriver         Driver;
			FReplicationClient Replication;
		};
		std::vector<std::unique_ptr<FClient>> Clients;

		FNetFixture()
		{
			RegisterNetworkTypes();
			BuildLevel(ServerScene);
			ServerDriver.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session(), true);
			Server.Begin(ServerScene, ServerDriver);
			ServerDriver.OnPlayerJoined = [this](const FNetDriver::FRemotePlayer& Player) { Server.OnPlayerJoined(Player.Connection); };
		}

		FClient& Join()
		{
			auto& Client = *Clients.emplace_back(std::make_unique<FClient>());
			BuildLevel(Client.Scene);
			Client.Replication.Begin(Client.Scene);
			Client.Driver.OnGameMessage = [&Client](FNetConnectionId, const std::vector<uint8>& Message) { Client.Replication.HandleMessage(Message); };
			Client.Driver.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Session());
			Pump();
			return Client;
		}

		// 서버 복제 한 번 + 메시지 왕복
		void Pump(int32 Rounds = 3)
		{
			for (int32 Round = 0; Round < Rounds; ++Round)
			{
				ServerDriver.Update(SendStep);
				Server.Tick(SendStep);
				for (auto& Client : Clients)
				{
					Client->Driver.Update(SendStep);
				}
			}
		}
	};
} // namespace

E_TEST(Replication_StaticNetIdsMatchAndValuesReplicate)
{
	FNetFixture Net;
	auto&       Client = Net.Join();
	E_EXPECT_TRUE(Client.Driver.GetClientState() == FNetDriver::EClientState::Joined);

	// 같은 씬이면 같은 정적 NetId
	const FEntity ServerCrate = FindByName(Net.ServerScene, "Crate");
	const FEntity ClientCrate = FindByName(Client.Scene, "Crate");
	E_EXPECT_EQ(NetReplication::GetNetId(Net.ServerScene, ServerCrate), 1u);
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, ClientCrate), 1u);
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, FindByName(Client.Scene, "Door")), 2u);
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, FindByName(Client.Scene, "Sun")), InvalidNetId);
	Client.Replication.ConsumeAssetsChanged();

	// 서버에서 값 변경 → 클라이언트 반영 (에셋 경로 컴포넌트가 바뀌면 다시 해석 필요 표시)
	Net.ServerScene.GetTransform(ServerCrate).Position                              = FVector3(100.0f, 0.0f, 50.0f);
	Net.ServerScene.GetRegistry().Get<FStaticMeshComponent>(ServerCrate).MeshAsset = "primitive:sphere";
	Net.ServerScene.GetRegistry().Get<FNameComponent>(ServerCrate).Name           = "BigCrate";
	Net.Pump();
	E_EXPECT_EQUALS(Client.Scene.GetTransform(ClientCrate).Position, FVector3(100.0f, 0.0f, 50.0f), 1.0e-4f);
	E_EXPECT_TRUE(Client.Scene.GetRegistry().Get<FStaticMeshComponent>(ClientCrate).MeshAsset == "primitive:sphere");
	E_EXPECT_TRUE(Client.Scene.GetRegistry().Get<FNameComponent>(ClientCrate).Name == "BigCrate");
	E_EXPECT_TRUE(Client.Replication.ConsumeAssetsChanged());

	// 컴포넌트 제거도 반영
	Net.ServerScene.GetRegistry().Remove<FStaticMeshComponent>(ServerCrate);
	Net.Pump();
	E_EXPECT_FALSE(Client.Scene.GetRegistry().Has<FStaticMeshComponent>(ClientCrate));

	// 복제 표시가 없는 엔티티는 보내지 않는다
	Net.ServerScene.GetTransform(FindByName(Net.ServerScene, "Sun")).Position = FVector3(1.0f, 2.0f, 3.0f);
	Net.Pump();
	E_EXPECT_EQUALS(Client.Scene.GetTransform(FindByName(Client.Scene, "Sun")).Position, FVector3(), 1.0e-6f);
}

E_TEST(Replication_DynamicSpawnDestroyAndEntityReferences)
{
	FNetFixture Net;
	auto&       Client = Net.Join();

	// 실행 중 생성: 부모(정적 Crate) 아래, 엔티티 참조 프로퍼티는 NetId로 옮겨진다
	const FEntity ServerCrate = FindByName(Net.ServerScene, "Crate");
	const FEntity Spawned     = Net.ServerScene.CreateEntity("Projectile");
	Net.ServerScene.SetParent(Spawned, ServerCrate);
	Net.ServerScene.GetRegistry().Emplace<FReplicatedComponent>(Spawned).OwnerPlayerId = 3;
	Net.ServerScene.GetTransform(Spawned).Position                                    = FVector3(0.0f, 10.0f, 0.0f);
	FSocketAttachmentComponent& Attach = Net.ServerScene.GetRegistry().Emplace<FSocketAttachmentComponent>(Spawned);
	Attach.Target                      = FindByName(Net.ServerScene, "Door");
	Attach.Socket                      = "Hinge";
	Net.Pump();

	const FEntity ClientProjectile = FindByName(Client.Scene, "Projectile");
	E_EXPECT_TRUE(ClientProjectile.IsValid());
	E_EXPECT_TRUE(Client.Scene.GetParent(ClientProjectile) == FindByName(Client.Scene, "Crate"));
	E_EXPECT_TRUE(NetReplication::GetNetId(Client.Scene, ClientProjectile) >= DynamicNetIdBase);
	E_EXPECT_EQ(Client.Scene.GetRegistry().Get<FReplicatedComponent>(ClientProjectile).OwnerPlayerId, 3);
	E_EXPECT_EQUALS(Client.Scene.GetTransform(ClientProjectile).Position, FVector3(0.0f, 10.0f, 0.0f), 1.0e-4f);
	const FSocketAttachmentComponent* ClientAttach = Client.Scene.GetRegistry().TryGet<FSocketAttachmentComponent>(ClientProjectile);
	E_EXPECT_TRUE(ClientAttach != nullptr && ClientAttach->Target == FindByName(Client.Scene, "Door") && ClientAttach->Socket == "Hinge");
	E_EXPECT_EQ(Net.Server.GetReplicatedCount(), 3u);

	// 파괴 → 클라이언트에서도 사라진다
	Net.ServerScene.DestroyEntity(Spawned);
	Net.Pump();
	E_EXPECT_FALSE(FindByName(Client.Scene, "Projectile").IsValid());
	E_EXPECT_EQ(Net.Server.GetReplicatedCount(), 2u);
}

E_TEST(Replication_LateJoinerGetsCurrentState)
{
	FNetFixture Net;
	auto&       Early = Net.Join();

	const FEntity Spawned = Net.ServerScene.CreateEntity("Pickup");
	Net.ServerScene.GetRegistry().Emplace<FReplicatedComponent>(Spawned);
	Net.ServerScene.GetTransform(Spawned).Position                                 = FVector3(5.0f, 5.0f, 5.0f);
	Net.ServerScene.GetTransform(FindByName(Net.ServerScene, "Door")).Position = FVector3(0.0f, 0.0f, 300.0f);
	Net.Pump();
	E_EXPECT_TRUE(FindByName(Early.Scene, "Pickup").IsValid());

	// 나중에 들어온 클라이언트: 동적 엔티티 생성 + 바뀐 정적 값
	auto& Late = Net.Join();
	E_EXPECT_TRUE(FindByName(Late.Scene, "Pickup").IsValid());
	E_EXPECT_EQUALS(Late.Scene.GetTransform(FindByName(Late.Scene, "Pickup")).Position, FVector3(5.0f, 5.0f, 5.0f), 1.0e-4f);
	E_EXPECT_EQUALS(Late.Scene.GetTransform(FindByName(Late.Scene, "Door")).Position, FVector3(0.0f, 0.0f, 300.0f), 1.0e-4f);
}

E_TEST(Replication_PrefabSpawnLinksChildren)
{
	// 프리팹 인스턴스는 경로로 생성하고, 복제 대상 하위 엔티티는 링크 ID로 NetId를 짝짓는다
	const std::filesystem::path Content = std::filesystem::temp_directory_path() / L"ProjectEReplicationTests";
	std::filesystem::create_directories(Content);
	std::filesystem::remove(Content / L"Pawn.eprefab"); // 이전 실행 결과 (CreatePrefab은 기존 파일을 덮지 않는다)
	FPrefabLibrary::Get().SetContentDirectory(Content);
	{
		RegisterNetworkTypes();
		FScene        Authoring;
		const FEntity Pawn = Authoring.CreateEntity("Pawn");
		Authoring.GetRegistry().Emplace<FReplicatedComponent>(Pawn);
		const FEntity Gun = Authoring.CreateEntity("Gun");
		Authoring.SetParent(Gun, Pawn);
		Authoring.GetRegistry().Emplace<FReplicatedComponent>(Gun);
		std::string Error;
		E_EXPECT_TRUE(FPrefabLibrary::Get().CreatePrefab(Authoring, Pawn, Content / L"Pawn.eprefab", &Error));
		FPrefabLibrary::Get().Invalidate();
	}

	FNetFixture Net;
	auto&       Client = Net.Join();
	const FEntity ServerPawn = FPrefabLibrary::Get().Instantiate(Net.ServerScene, "Pawn.eprefab", NullEntity);
	E_EXPECT_TRUE(ServerPawn.IsValid());
	Net.ServerScene.GetTransform(ServerPawn).Position = FVector3(0.0f, 0.0f, 90.0f);
	const FEntity ServerGun = FindByName(Net.ServerScene, "Gun");
	Net.ServerScene.GetTransform(ServerGun).Position = FVector3(20.0f, 0.0f, 0.0f);
	Net.Pump();

	const FEntity ClientPawn = FindByName(Client.Scene, "Pawn");
	const FEntity ClientGun  = FindByName(Client.Scene, "Gun");
	E_EXPECT_TRUE(ClientPawn.IsValid() && ClientGun.IsValid());
	E_EXPECT_TRUE(FPrefabLibrary::IsInstanceRoot(Client.Scene, ClientPawn)); // 클라이언트도 프리팹 인스턴스로 만들어졌다
	E_EXPECT_TRUE(Client.Scene.GetParent(ClientGun) == ClientPawn);
	E_EXPECT_EQ(NetReplication::GetNetId(Client.Scene, ClientGun), NetReplication::GetNetId(Net.ServerScene, ServerGun));
	E_EXPECT_EQUALS(Client.Scene.GetTransform(ClientPawn).Position, FVector3(0.0f, 0.0f, 90.0f), 1.0e-4f);
	E_EXPECT_EQUALS(Client.Scene.GetTransform(ClientGun).Position, FVector3(20.0f, 0.0f, 0.0f), 1.0e-4f);
	FPrefabLibrary::Get().SetContentDirectory(std::filesystem::path());
}

#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsReflection.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <algorithm>
#include <deque>
#include <memory>

// 캐릭터 클라이언트 예측 (World/GameWorldCharacter.cpp): 루프백 서버 1 + 클라이언트 1, 양쪽 모두 같은 씬 (정적 NetId)
namespace
{
	constexpr float Step = 1.0f / 60.0f;

	FEntity BuildLevel(FScene& Scene)
	{
		const FEntity Floor = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(3000.0f, 3000.0f, 10.0f);
		Scene.GetRegistry().Emplace<FRigidBodyComponent>(Floor).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);
		const FEntity Pawn = Scene.CreateEntity("Pawn");
		Scene.GetTransform(Pawn).Position = FVector3(0.0f, 0.0f, 91.0f);
		Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Pawn);
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Pawn);
		Scene.UpdateTransforms();
		NetReplication::AssignStaticNetIds(Scene);
		return Pawn;
	}

	struct FSide
	{
		FScene         Scene;
		FScriptSystem  Scripts;
		FPhysicsSystem Physics;
		FNetDriver     Net;
		FGameWorld     World;
		FEntity        Pawn;
	};
} // namespace

E_TEST(CharacterPrediction_ImmediateMatchAndCorrection)
{
	RegisterPhysicsTypes();
	RegisterNetworkTypes();
	auto            Hub = std::make_shared<FLoopbackHub>();
	FNetSessionInfo Session;

	auto Server  = std::make_unique<FSide>();
	Server->Pawn = BuildLevel(Server->Scene);
	Server->World.Init({ &Server->Scripts, &Server->Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Server->Net });
	Server->Net.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
	Server->Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
		Server->Scene.GetRegistry().Get<FReplicatedComponent>(Server->Pawn).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
	};
	Server->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Server->World.HandleNetMessage(Connection, Message); };
	Server->World.BeginPlay(Server->Scene, ENetMode::DedicatedServer);

	auto Client  = std::make_unique<FSide>();
	Client->Pawn = BuildLevel(Client->Scene);
	Client->World.Init({ &Client->Scripts, &Client->Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Client->Net });
	Client->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Client->World.HandleNetMessage(Connection, Message); };
	Client->World.BeginPlay(Client->Scene, ENetMode::Client);
	Client->Net.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Session);

	FInput     ClientInput;
	const auto Pump = [&](int32 Rounds, const FVector3& Direction) {
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			if (Direction.LengthSquared() > 0.0f)
			{
				Client->Physics.AddMovementInput(Client->Pawn, Direction); // 스크립트의 entity:AddMovementInput과 같은 경로
			}
			Client->World.TickGameplay(Step, &ClientInput);
			Client->Net.Update(Step);
			Server->Net.Update(Step);
			Server->World.TickGameplay(Step, nullptr);
			Server->Net.Update(Step);
			Client->Net.Update(Step);
		}
	};
	Pump(5, FVector3());
	E_EXPECT_TRUE(Client->Net.GetClientState() == FNetDriver::EClientState::Joined);
	// 복제 대신: 클라이언트 씬의 소유자도 서버와 같게 (실제로는 ReplicatedComponent 복제로 온다)
	Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = static_cast<int32>(Client->Net.GetLocalPlayerId());
	E_EXPECT_TRUE(Client->World.IsPredicted(Client->Pawn));
	E_EXPECT_FALSE(Server->World.IsPredicted(Server->Pawn));
	Pump(60, FVector3()); // 양쪽 착지
	const float StartX = Client->Scene.GetTransform(Client->Pawn).Position.X;
	E_EXPECT_NEAR(Server->Scene.GetTransform(Server->Pawn).Position.Z, 90.0f, 2.0f);

	// 1) 예측: 클라이언트 틱 한 번에 바로 움직인다 (서버는 아직 그대로)
	Client->Physics.AddMovementInput(Client->Pawn, FVector3(1.0f, 0.0f, 0.0f));
	Client->World.TickGameplay(Step, &ClientInput);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.X - StartX, 450.0f * Step, 0.5f);
	E_EXPECT_NEAR(Server->Scene.GetTransform(Server->Pawn).Position.X, StartX, 0.01f);

	// 2) 서버도 같은 무브로 같은 곳에 (재조정 없음)
	Pump(60, FVector3(1.0f, 0.0f, 0.0f));
	Pump(10, FVector3());
	const float ClientX = Client->Scene.GetTransform(Client->Pawn).Position.X;
	const float ServerX = Server->Scene.GetTransform(Server->Pawn).Position.X;
	E_EXPECT_TRUE(ClientX - StartX > 400.0f);
	E_EXPECT_NEAR(ClientX, ServerX, 1.0f);
	E_EXPECT_EQ(Client->World.GetCharacterCorrectionCount(), 0u);

	// 3) 서버만 아는 일 (순간이동): 다음 ack로 클라이언트가 서버 위치로 보정된다
	FCharacterState Teleport = Server->Physics.GetCharacterState(Server->Pawn);
	Teleport.Position.Y += 300.0f;
	Server->Physics.SetCharacterState(Server->Scene, Server->Pawn, Teleport);
	Pump(10, FVector3(1.0f, 0.0f, 0.0f)); // 움직이는 중에 (ack가 계속 온다)
	Pump(10, FVector3());
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.Y, Server->Scene.GetTransform(Server->Pawn).Position.Y, 1.0f);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.Y, 300.0f, 2.0f);
	E_EXPECT_TRUE(Client->World.GetCharacterCorrectionCount() >= 1u);

	// 4) 예측 끔 (캐릭터 컴포넌트): 클라이언트는 미리 움직이지 않고 보내기만, 서버는 그대로 움직인다
	Client->Scene.GetRegistry().Get<FCharacterMovementComponent>(Client->Pawn).bClientPrediction = false;
	E_EXPECT_FALSE(Client->World.IsPredicted(Client->Pawn));
	const float    ClientBefore = Client->Scene.GetTransform(Client->Pawn).Position.X;
	const float    ServerBefore = Server->Scene.GetTransform(Server->Pawn).Position.X;
	const uint32   Corrections  = Client->World.GetCharacterCorrectionCount();
	Client->Physics.AddMovementInput(Client->Pawn, FVector3(1.0f, 0.0f, 0.0f));
	Client->World.TickGameplay(Step, &ClientInput);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.X, ClientBefore, 0.01f); // 즉시 움직이지 않는다
	Pump(30, FVector3(1.0f, 0.0f, 0.0f));
	E_EXPECT_TRUE(Server->Scene.GetTransform(Server->Pawn).Position.X - ServerBefore > 200.0f); // 서버는 무브로 움직였다
	E_EXPECT_EQ(Client->World.GetCharacterCorrectionCount(), Corrections);                      // 재조정 없음

	// 프로젝트 설정으로 끄면 컴포넌트가 켜져 있어도 끔
	Client->Scene.GetRegistry().Get<FCharacterMovementComponent>(Client->Pawn).bClientPrediction = true;
	E_EXPECT_TRUE(Client->World.IsPredicted(Client->Pawn));
	FProjectSettings::Get().Network.bClientPrediction = false;
	E_EXPECT_FALSE(Client->World.IsPredicted(Client->Pawn));
	FProjectSettings::Get().Network.bClientPrediction = true;

	Client->World.EndPlay();
	Server->World.EndPlay();
	Client->Net.Shutdown();
	Server->Net.Shutdown();
}

// 3D 넉백/발사 (World/GameWorldCharacter.cpp 머리 주석 — 2D와 같은 규칙): 클라이언트가 자기 캐릭터에 건 발사는 무브와 함께 가서 같은 결과(재조정 없음),
// 서버가 원격 캐릭터에 준 넉백은 다음 받은 무브에 실려 ack 상태(속도·경직)로 와 재조정으로 같은 궤적에 수렴, 클라이언트가 남의 캐릭터에 건 것은 무시
E_TEST(CharacterPrediction_KnockbackMatchesServer)
{
	RegisterPhysicsTypes();
	RegisterNetworkTypes();
	auto            Hub = std::make_shared<FLoopbackHub>();
	FNetSessionInfo Session;
	std::deque<std::pair<int32, std::vector<uint8>>> DelayedAcks; // ack 6라운드(100ms) 지연
	int32                                       Round = 0;

	auto Server  = std::make_unique<FSide>();
	Server->Pawn = BuildLevel(Server->Scene);
	Server->World.Init({ &Server->Scripts, &Server->Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Server->Net });
	Server->Net.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7783, Session, true);
	Server->Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
		Server->Scene.GetRegistry().Get<FReplicatedComponent>(Server->Pawn).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
	};
	Server->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Server->World.HandleNetMessage(Connection, Message); };
	Server->World.BeginPlay(Server->Scene, ENetMode::DedicatedServer);

	auto Client  = std::make_unique<FSide>();
	Client->Pawn = BuildLevel(Client->Scene);
	Client->World.Init({ &Client->Scripts, &Client->Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Client->Net });
	Client->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
		if (!Message.empty() && Message[0] == static_cast<uint8>(ENetMessageType::CharacterAck))
		{
			DelayedAcks.emplace_back(Round + 6, Message);
			return;
		}
		Client->World.HandleNetMessage(Connection, Message);
	};
	Client->World.BeginPlay(Client->Scene, ENetMode::Client);
	Client->Net.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7783", Session);

	FInput     ClientInput;
	const auto Pump = [&](float InputX) {
		if (InputX != 0.0f)
		{
			Client->Physics.AddMovementInput(Client->Pawn, FVector3(InputX, 0.0f, 0.0f));
		}
		Client->World.TickGameplay(Step, &ClientInput);
		Client->Net.Update(Step);
		Server->Net.Update(Step);
		Server->World.TickGameplay(Step, nullptr);
		Server->Net.Update(Step);
		Client->Net.Update(Step);
		++Round;
		while (!DelayedAcks.empty() && DelayedAcks.front().first <= Round)
		{
			Client->World.HandleNetMessage(0, DelayedAcks.front().second);
			DelayedAcks.pop_front();
		}
	};
	const auto PumpN = [&](int32 Count, float InputX) {
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Pump(InputX);
		}
	};
	PumpN(5, 0.0f);
	E_EXPECT_TRUE(Client->Net.GetClientState() == FNetDriver::EClientState::Joined);
	Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = static_cast<int32>(Client->Net.GetLocalPlayerId());
	PumpN(60, 0.0f); // 착지 + ack 정착
	const auto ClientPos = [&]() { return Client->Scene.GetTransform(Client->Pawn).Position; };
	const auto ServerPos = [&]() { return Server->Scene.GetTransform(Server->Pawn).Position; };
	const uint32 SettledCorrections = Client->World.GetCharacterCorrectionCount();

	// 1) 클라이언트가 자기 캐릭터에 건 발사: 무브와 함께 서버로 → 같은 결과, 재조정 없음
	Client->Physics.LaunchCharacter(Client->Pawn, FVector3(200.0f, 0.0f, 500.0f), false, true);
	Pump(1.0f);
	Pump(1.0f);
	E_EXPECT_FALSE(Client->Physics.IsGrounded(Client->Pawn)); // 예측: 바로 떴다
	PumpN(80, 1.0f);
	PumpN(12, 0.0f);
	E_EXPECT_NEAR(ClientPos().X, ServerPos().X, 0.5f);
	E_EXPECT_NEAR(ClientPos().Z, ServerPos().Z, 0.5f);
	E_EXPECT_EQ(Client->World.GetCharacterCorrectionCount(), SettledCorrections);

	// 2) 서버가 준 넉백 (경직 0.4초): 클라이언트는 계속 +X 입력 — 서버는 넉백 무브부터 -X로 밀리고 경직 동안 입력을 무시
	PumpN(20, 1.0f);
	const float BeforeX = ServerPos().X;
	Server->Physics.AddKnockback(Server->Pawn, FVector3(-900.0f, 0.0f, 400.0f), 0.4f);
	float MinServerX = BeforeX;
	for (int32 Index = 0; Index < 24; ++Index)
	{
		Pump(1.0f);
		MinServerX = std::min(MinServerX, ServerPos().X);
	}
	E_EXPECT_TRUE(MinServerX < BeforeX - 100.0f); // 입력과 반대로 밀렸다
	PumpN(60, 1.0f);
	PumpN(20, 0.0f);
	E_EXPECT_NEAR(ClientPos().X, ServerPos().X, 1.0f);
	E_EXPECT_NEAR(ClientPos().Z, ServerPos().Z, 1.0f);
	E_EXPECT_TRUE(Client->World.GetCharacterCorrectionCount() > SettledCorrections);
	E_EXPECT_FALSE(Server->Physics.IsCharacterStunned(Server->Pawn));
	// 넉백 뒤로는 다시 같은 무브 → 같은 결과
	const uint32 Corrections = Client->World.GetCharacterCorrectionCount();
	PumpN(40, 1.0f);
	PumpN(12, 0.0f);
	E_EXPECT_EQ(Client->World.GetCharacterCorrectionCount(), Corrections);
	E_EXPECT_NEAR(ClientPos().X, ServerPos().X, 0.5f);

	// 3) 클라이언트에서 남의 캐릭터(서버 소유)에 건 넉백은 무시 — 소유권을 잠시 서버로
	Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = -1;
	Client->Physics.AddKnockback(Client->Pawn, FVector3(-900.0f, 0.0f, 0.0f), 0.5f);
	Client->World.TickGameplay(Step, &ClientInput);
	E_EXPECT_FALSE(Client->Physics.HasPendingLaunch(Client->Pawn));
	E_EXPECT_FALSE(Client->Physics.IsCharacterStunned(Client->Pawn));

	Client->World.EndPlay();
	Server->World.EndPlay();
	Client->Net.Shutdown();
	Server->Net.Shutdown();
}

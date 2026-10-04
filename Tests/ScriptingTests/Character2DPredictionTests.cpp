// 2D 캐릭터 클라이언트 예측 (World/GameWorldCharacter2D.cpp): 루프백 서버 1 + 클라이언트 1, 양쪽 같은 씬 (정적 NetId).
// 예측이 바로 움직이고 서버 결과와 일치(재조정 없음), 손실·지연(메시지를 버리고 ack를 늦춤)에도 겹쳐 보낸 무브로 일치, 서버만 아는 일은 보정
#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/PhysicsReflection.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <deque>
#include <memory>

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	FEntity BuildLevel(FScene& Scene)
	{
		const FEntity Floor = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
		Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(20000.0f, 100.0f);
		const FEntity Ledge = Scene.CreateEntity("Ledge"); // 원웨이 발판 (윗면 Z = 150)
		Scene.GetTransform(Ledge).Position = FVector3(800.0f, 0.0f, 140.0f);
		FBoxCollider2DComponent& LedgeBox  = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Ledge);
		LedgeBox.Size                      = FVector2(600.0f, 20.0f);
		LedgeBox.bOneWay                   = true;
		const FEntity Pawn = Scene.CreateEntity("Pawn");
		Scene.GetTransform(Pawn).Position = FVector3(0.0f, 0.0f, 61.0f);
		Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Pawn);
		Scene.GetRegistry().Emplace<FReplicatedComponent>(Pawn);
		Scene.UpdateTransforms();
		NetReplication::AssignStaticNetIds(Scene);
		return Pawn;
	}

	struct FSide
	{
		FScene        Scene;
		FScriptSystem Scripts;
		FNetDriver    Net;
		FGameWorld    World;
		FEntity       Pawn;
	};

	// 클라이언트 입력 한 프레임 (스크립트의 entity:AddMovementInput/Jump/StopJumping/Dash/DropDown과 같은 경로)
	struct FInputFrame
	{
		float InputX = 0.0f;
		bool  bJump = false, bRelease = false, bDash = false, bDrop = false;
	};
} // namespace

E_TEST(Character2DPrediction_MatchesServerUnderLossAndLag)
{
	RegisterPhysicsTypes();
	RegisterNetworkTypes();
	auto            Hub = std::make_shared<FLoopbackHub>();
	FNetSessionInfo Session;

	// 불량 망 흉내: 무브 패킷 3개 중 1개, ack 4개 중 1개를 버리고, 남은 ack는 6라운드(100ms) 늦게 넘긴다
	uint32                                      MoveCounter = 0, AckCounter = 0;
	std::deque<std::pair<int32, std::vector<uint8>>> DelayedAcks;
	int32                                       Round = 0;

	auto Server  = std::make_unique<FSide>();
	Server->Pawn = BuildLevel(Server->Scene);
	Server->World.Init({ &Server->Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Server->Net });
	Server->Net.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7779, Session, true);
	Server->Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
		Server->Scene.GetRegistry().Get<FReplicatedComponent>(Server->Pawn).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
	};
	bool bLossy = false;
	Server->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
		if (bLossy && !Message.empty() && Message[0] == static_cast<uint8>(ENetMessageType::CharacterMoves2D) && (MoveCounter++ % 3) == 1)
		{
			return;
		}
		Server->World.HandleNetMessage(Connection, Message);
	};
	Server->World.BeginPlay(Server->Scene, ENetMode::DedicatedServer);

	auto Client  = std::make_unique<FSide>();
	Client->Pawn = BuildLevel(Client->Scene);
	Client->World.Init({ &Client->Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Client->Net });
	Client->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
		if (bLossy && !Message.empty() && Message[0] == static_cast<uint8>(ENetMessageType::CharacterAck2D))
		{
			if ((AckCounter++ % 4) == 2)
			{
				return;
			}
			DelayedAcks.emplace_back(Round + 6, Message);
			return;
		}
		Client->World.HandleNetMessage(Connection, Message);
	};
	Client->World.BeginPlay(Client->Scene, ENetMode::Client);
	Client->Net.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7779", Session);

	FInput                      ClientInput;
	FCharacterMovement2DSystem& Moves = Client->World.GetCharacters2D();
	const auto Pump = [&](const FInputFrame& Frame) {
		if (Frame.InputX != 0.0f)
		{
			Moves.AddMovementInput(Client->Pawn, FVector3(Frame.InputX, 0.0f, 0.0f));
		}
		if (Frame.bJump)
		{
			Moves.Jump(Client->Pawn);
		}
		if (Frame.bRelease)
		{
			Moves.StopJumping(Client->Pawn);
		}
		if (Frame.bDash)
		{
			Moves.Dash(Client->Pawn, FVector3(1.0f, 0.0f, 0.3f));
		}
		if (Frame.bDrop)
		{
			Moves.DropDown(Client->Pawn);
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
	const auto PumpN = [&](int32 Count, const FInputFrame& Frame) {
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Pump(Frame);
		}
	};
	PumpN(5, {});
	E_EXPECT_TRUE(Client->Net.GetClientState() == FNetDriver::EClientState::Joined);
	Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = static_cast<int32>(Client->Net.GetLocalPlayerId());
	E_EXPECT_TRUE(Client->World.IsPredicted(Client->Pawn));
	E_EXPECT_FALSE(Server->World.IsPredicted(Server->Pawn));
	PumpN(30, {}); // 착지
	const float StartX = Client->Scene.GetTransform(Client->Pawn).Position.X;
	E_EXPECT_NEAR(Server->Scene.GetTransform(Server->Pawn).Position.Z, 60.0f, 1.5f);

	// 1) 예측: 클라이언트 틱 한 번에 바로 움직인다 (서버는 아직)
	Moves.AddMovementInput(Client->Pawn, FVector3(1.0f, 0.0f, 0.0f));
	Client->World.TickGameplay(Step, &ClientInput);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.X - StartX, 100.0f * Step, 0.5f); // 가속 6000 × dt × dt
	E_EXPECT_NEAR(Server->Scene.GetTransform(Server->Pawn).Position.X, StartX, 0.01f);

	// 2) 달리기 + 점프(가변) + 2단 점프 + 원웨이 착지 + 대시 + 내려가기: 서버가 같은 무브로 같은 곳에 (재조정 없음)
	const auto RunScript = [&]() {
		PumpN(20, { 1.0f });
		Pump({ 1.0f, true });
		PumpN(5, { 1.0f });
		Pump({ 1.0f, false, true });
		PumpN(8, { 1.0f });
		Pump({ 1.0f, true });  // 2단 점프
		PumpN(40, { 0.3f });   // 원웨이 발판 쪽으로
		Pump({ 0.0f, false, false, true }); // 대시
		PumpN(30, {});
		Pump({ 0.0f, false, false, false, true }); // 내려가기
		PumpN(40, {});
	};
	RunScript();
	PumpN(10, {});
	const FVector3 ClientEnd = Client->Scene.GetTransform(Client->Pawn).Position;
	const FVector3 ServerEnd = Server->Scene.GetTransform(Server->Pawn).Position;
	E_EXPECT_TRUE(ClientEnd.X - StartX > 300.0f);
	E_EXPECT_NEAR(ClientEnd.X, ServerEnd.X, 0.5f);
	E_EXPECT_NEAR(ClientEnd.Z, ServerEnd.Z, 0.5f);
	E_EXPECT_EQ(Client->World.GetCharacterCorrectionCount(), 0u);

	// 3) 불량 망 (손실 + 지연): 겹쳐 보낸 무브로 서버가 빠짐없이 같은 무브를 적용 → 여전히 일치, 재조정 없음
	bLossy = true;
	RunScript();
	PumpN(30, {});
	bLossy = false;
	PumpN(10, {});
	E_EXPECT_TRUE(MoveCounter > 20u && AckCounter > 20u);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.X, Server->Scene.GetTransform(Server->Pawn).Position.X, 0.5f);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.Z, Server->Scene.GetTransform(Server->Pawn).Position.Z, 0.5f);
	E_EXPECT_EQ(Client->World.GetCharacterCorrectionCount(), 0u);

	// 4) 서버만 아는 일 (순간이동): 다음 ack로 클라이언트가 서버 위치로 보정된다
	FCharacterState2D Teleport = Server->World.GetCharacters2D().GetState(Server->Pawn);
	Teleport.Position.X += 500.0f;
	Server->World.GetCharacters2D().SetState(Server->Scene, Server->Pawn, Teleport);
	PumpN(10, { 1.0f });
	PumpN(30, {});
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.X, Server->Scene.GetTransform(Server->Pawn).Position.X, 1.0f);
	E_EXPECT_TRUE(Client->World.GetCharacterCorrectionCount() >= 1u);

	// 5) 예측 끔: 클라이언트는 미리 움직이지 않는다
	Client->Scene.GetRegistry().Get<FCharacterMovement2DComponent>(Client->Pawn).bClientPrediction = false;
	E_EXPECT_FALSE(Client->World.IsPredicted(Client->Pawn));
	const float Before = Client->Scene.GetTransform(Client->Pawn).Position.X;
	Moves.AddMovementInput(Client->Pawn, FVector3(1.0f, 0.0f, 0.0f));
	Client->World.TickGameplay(Step, &ClientInput);
	E_EXPECT_NEAR(Client->Scene.GetTransform(Client->Pawn).Position.X, Before, 0.01f);
	Client->Scene.GetRegistry().Get<FCharacterMovement2DComponent>(Client->Pawn).bClientPrediction = true;

	Client->World.EndPlay();
	Server->World.EndPlay();
	Client->Net.Shutdown();
	Server->Net.Shutdown();
}

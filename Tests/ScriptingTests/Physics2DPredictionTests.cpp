// 2D 물리 예측 (World/GameWorldPhysicsPrediction2D.cpp): 루프백 전용 서버 1 + 클라이언트 1, 양쪽 같은 씬 (정적 NetId).
// 클라이언트 2D 캐릭터가 2D 상자를 미는 장면에서 예측을 켠 경우와 끈 경우를 비교하고(3D PhysicsPrediction_* 와 같은 방식),
// 손실·지연(루프백 시뮬레이션)에서도 서버 결과로 수렴하는지 본다
// 재조정 겹침 거부(ReceiveCharacterAck2D ②)가 실제 네트워크 장면에서 일어나고 그 뒤 튀지 않는지도 본다 (Physics2DPrediction_ReplayOverlapRejectInNetwork)
#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/PhysicsReflection.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <cmath>
#include <cstdio>

#include <memory>

namespace
{
	constexpr float Step   = 1.0f / 60.0f;
	constexpr float CrateX = 150.0f;

	void BuildLevel(FScene& Scene, FEntity& OutPawn, FEntity& OutCrate)
	{
		FRegistry&    Registry = Scene.GetRegistry();
		const FEntity Floor    = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position                       = FVector3(0.0f, 0.0f, -50.0f);
		Registry.Emplace<FBoxCollider2DComponent>(Floor).Size    = FVector2(20000.0f, 100.0f);
		OutPawn                              = Scene.CreateEntity("Pawn");
		Scene.GetTransform(OutPawn).Position = FVector3(0.0f, 0.0f, 61.0f);
		Registry.Emplace<FCharacterMovement2DComponent>(OutPawn);
		Registry.Emplace<FReplicatedComponent>(OutPawn);
		OutCrate                              = Scene.CreateEntity("Crate");
		Scene.GetTransform(OutCrate).Position = FVector3(CrateX, 0.0f, 40.0f);
		Registry.Emplace<FBoxCollider2DComponent>(OutCrate).Size = FVector2(80.0f, 80.0f);
		Registry.Emplace<FRigidBody2DComponent>(OutCrate).Mass   = 20.0f;
		Registry.Emplace<FReplicatedComponent>(OutCrate);
		Scene.UpdateTransforms();
	}

	struct FServerSide
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationServer Replication;
		FEntity            Pawn, Crate;
	};

	struct FClientSide
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationClient Replication;
		FEntity            Pawn, Crate;
	};

	struct FPushResult
	{
		bool   bPredictedNearCrate    = false;
		bool   bDynamicOnClient       = false;
		int32  ContactFrame           = -1;
		int32  CrateMoveFrame         = -1;
		uint32 Corrections            = 0;
		uint32 OverlapRejects         = 0; // 재조정 ② 결과 겹침 거부 (밀기에서는 없어야 한다 — 바디 옆은 겹침이 생기지 않는 자리)
		float  MaxCrateJump           = 0.0f;
		float  FinalDifference        = 0.0f; // 멈춘 뒤 클라이언트 ↔ 서버 상자 위치 차이
		float  ServerCrateTravel      = 0.0f; // 서버에서 상자가 밀린 거리
		bool   bReleased              = false;
		bool   bKinematicAfterRelease = false;
	};

	// LatencyMs/LossPercent > 0이면 루프백 시뮬레이션 (양쪽 보내는 쪽, 시드 고정)
	FPushResult RunPushScenario(int32 LatencyMs, float LossPercent)
	{
		RegisterPhysicsTypes();
		RegisterNetworkTypes();
		auto            Hub = std::make_shared<FLoopbackHub>();
		FNetSessionInfo Session;
		FPushResult     Result;
		// 한 프레임에 각 쪽 Net.Update를 두 번 부르므로 Poll 한 번 = 반 프레임
		const auto MakeTransport = [&](uint64 Seed) {
			auto Transport = std::make_unique<FLoopbackTransport>(Hub);
			Transport->SetSimulationSeed(Seed);
			Transport->SetPollIntervalMs(1000.0 / 120.0);
			Transport->SetSimulation(LatencyMs, LossPercent);
			return Transport;
		};

		auto Server = std::make_unique<FServerSide>();
		BuildLevel(Server->Scene, Server->Pawn, Server->Crate);
		Server->World.Init({ &Server->Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Server->Net });
		Server->Net.StartServer(MakeTransport(11), 7781, Session, true);
		Server->Replication.Begin(Server->Scene, Server->Net);
		Server->Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
			Server->Scene.GetRegistry().Get<FReplicatedComponent>(Server->Pawn).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
			Server->Replication.OnPlayerJoined(Player.Connection);
		};
		Server->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Server->World.HandleNetMessage(Connection, Message); };
		Server->World.BeginPlay(Server->Scene, ENetMode::DedicatedServer);

		auto Client = std::make_unique<FClientSide>();
		BuildLevel(Client->Scene, Client->Pawn, Client->Crate);
		Client->World.Init({ &Client->Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Client->Net });
		Client->Replication.Begin(Client->Scene);
		Client->Replication.SetTransformFilter([&](FEntity Entity) { return !Client->World.IsPredicted(Entity); }); // 런타임과 같은 연결
		Client->World.SetReplicationClient(&Client->Replication);
		Client->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
			if (!Client->Replication.HandleMessage(Message))
			{
				Client->World.HandleNetMessage(Connection, Message);
			}
		};
		Client->World.BeginPlay(Client->Scene, ENetMode::Client);
		Client->Net.StartClient(MakeTransport(12), "127.0.0.1:7781", Session);

		FInput      ClientInput;
		int32       Frame = 0;
		FVector3    CratePrevious, CrateCurrent;
		int32       CrateSamples = 0;
		const float ServerCrateStart = Server->Scene.GetTransform(Server->Crate).Position.X;
		const auto  Pump = [&](int32 Rounds, float InputX) {
			for (int32 Round = 0; Round < Rounds; ++Round, ++Frame)
			{
				Client->Net.Update(Step);
				Client->Replication.Update(Step);
				if (InputX != 0.0f)
				{
					Client->World.GetCharacters2D().AddMovementInput(Client->Pawn, FVector3(InputX, 0.0f, 0.0f));
				}
				Client->World.TickGameplay(Step, &ClientInput);
				Client->Net.Update(Step);
				Server->Net.Update(Step);
				Server->World.TickGameplay(Step, nullptr);
				Server->Replication.Tick(Step);
				Server->Net.Update(Step);

				const FVector3 Crate = Client->Scene.GetTransform(Client->Crate).Position;
				const FVector3 Pawn  = Client->Scene.GetTransform(Client->Pawn).Position;
				if (Result.ContactFrame < 0 && CrateX - Pawn.X < 30.0f + 40.0f + 2.0f)
				{
					Result.ContactFrame = Frame;
				}
				if (Result.CrateMoveFrame < 0 && std::abs(Crate.X - CrateX) > 1.0f)
				{
					Result.CrateMoveFrame = Frame;
				}
				// 멈춰 있다가 출발하는 프레임은 뺀다 (FMotionTrack과 같은 규칙 — 키네마틱 대리 바디는 상자를 첫 프레임에 바로 걷는 속도로 민다)
				if (CrateSamples >= 2 && FVector3::DistanceSquared(CrateCurrent, CratePrevious) > 0.25f)
				{
					Result.MaxCrateJump = std::max(Result.MaxCrateJump, FVector3::Distance(Crate, CrateCurrent + (CrateCurrent - CratePrevious)));
				}
				CratePrevious = CrateCurrent;
				CrateCurrent  = Crate;
				++CrateSamples;
			}
		};
		Pump(10, 0.0f);
		E_EXPECT_TRUE(Client->Net.GetClientState() == FNetDriver::EClientState::Joined);
		Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = static_cast<int32>(Client->Net.GetLocalPlayerId());
		Pump(60, 0.0f); // 착지 + 시각 맞추기 (ack + 스냅샷)
		Result.bPredictedNearCrate = Client->World.IsPhysicsSimulatedLocally(Client->Crate);
		Result.bDynamicOnClient    = Client->World.GetPhysics2D().IsDynamicBody(Client->Crate);
		const uint32 Before        = Client->World.GetCharacterCorrectionCount();

		Pump(30, 1.0f);  // 0.5초 밀기
		Pump(150, 0.0f); // 멈추고 서버 상태로 수렴
		Result.Corrections       = Client->World.GetCharacterCorrectionCount() - Before;
		Result.OverlapRejects    = Client->World.GetReplayOverlapRejectCount2D();
		Result.FinalDifference   = FVector3::Distance(Client->Scene.GetTransform(Client->Crate).Position, Server->Scene.GetTransform(Server->Crate).Position);
		Result.ServerCrateTravel = Server->Scene.GetTransform(Server->Crate).Position.X - ServerCrateStart;

		// 멀어지면 (반경 × 1.25 밖, 1초 + 블렌드) 키네마틱 보간으로 돌아간다
		Pump(90, -1.0f);
		Pump(120, 0.0f);
		Result.bReleased              = !Client->World.IsPhysicsPredicted(Client->Crate);
		Result.bKinematicAfterRelease = !Client->World.GetPhysics2D().IsDynamicBody(Client->Crate);

		Client->World.EndPlay();
		Server->World.EndPlay();
		Client->Net.Shutdown();
		Server->Net.Shutdown();
		return Result;
	}

	// ---- 겹침 거부 장면 (Physics2DPrediction_ReplayOverlapRejectInNetwork): 마찰 없는 상자가 왼쪽으로 미끄러져 오고, 서버 게임 코드가 클라이언트 캐릭터를
	//   상자 앞 공중(LandX, 발이 상자 윗면 5cm 위)으로 순간이동시킨다. 서버 캐릭터는 바닥에 먼저 내려앉고 상자는 그 대리 캡슐에 막혀 멈춘다.
	//   클라이언트는 그 ack를 늦게 받는다 — 그동안 클라이언트 상자는 막는 캐릭터가 없어(클라이언트 캐릭터는 아직 원래 자리) LandX 밑까지 미끄러져 왔다.
	//   재조정 ②(무브마다 상자를 기록 자리에)는 서버처럼 바닥에 내려앉는데, 지금 자리 상자가 그 자리를 차지하므로 묻힌다 → 거부하고
	//   ①(지금 자리 상자 위에 내려앉음)을 쓴다. 뒤이은 스냅샷·ack가 상자(멈춤)·캐릭터(바닥)를 서버 쪽으로 맞춘다
	constexpr float EmbedCrateStartX = 420.0f;
	constexpr float EmbedCrateSpeed  = -290.0f; // cm/s (예측 진입 상한 300cm/s 아래)
	constexpr float LandX            = 150.0f;
	constexpr float TeleportCrateX   = 265.0f; // 서버 상자가 여기를 지나면 순간이동 (상자 왼쪽 면과 캐릭터 사이 45cm — 캐릭터가 먼저 내려앉는다)

	void BuildEmbedLevel(FScene& Scene, FEntity& OutPawn, FEntity& OutCrate)
	{
		FRegistry&    Registry = Scene.GetRegistry();
		const FEntity Floor    = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position                    = FVector3(0.0f, 0.0f, -50.0f);
		Registry.Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(20000.0f, 100.0f);
		const FEntity Wall                                    = Scene.CreateEntity("Wall"); // 튕겨 돌아간 상자가 멈추는 곳 (수렴 비교)
		Scene.GetTransform(Wall).Position                     = FVector3(560.0f, 0.0f, 100.0f);
		Registry.Emplace<FBoxCollider2DComponent>(Wall).Size  = FVector2(40.0f, 200.0f);
		OutPawn                                               = Scene.CreateEntity("Pawn");
		Scene.GetTransform(OutPawn).Position                  = FVector3(0.0f, 0.0f, 61.0f);
		Registry.Emplace<FCharacterMovement2DComponent>(OutPawn);
		Registry.Emplace<FReplicatedComponent>(OutPawn);
		OutCrate                              = Scene.CreateEntity("Crate");
		Scene.GetTransform(OutCrate).Position = FVector3(EmbedCrateStartX, 0.0f, 40.0f);
		FBoxCollider2DComponent& CrateBox     = Registry.Emplace<FBoxCollider2DComponent>(OutCrate);
		CrateBox.Size                         = FVector2(80.0f, 80.0f);
		CrateBox.Friction                     = 0.0f;
		FRigidBody2DComponent& CrateBody      = Registry.Emplace<FRigidBody2DComponent>(OutCrate);
		CrateBody.Mass                        = 20.0f;
		CrateBody.bFixedRotation              = true; // 미끄러지는 상자 (벽에 부딪혀 흔들리지 않게 — 수렴 비교)
		Registry.Emplace<FReplicatedComponent>(OutCrate);
		Scene.UpdateTransforms();
	}

	struct FEmbedResult
	{
		bool   bJoined              = false;
		bool   bPredictedCrate      = false; // 첫 거부 때 클라이언트가 상자를 예측 시뮬레이션 중
		bool   bServerOnFloor       = false; // 서버 캐릭터는 바닥 (상자보다 먼저 내려앉음)
		uint32 OverlapRejects       = 0;
		int32  FirstRejectFrame     = -1;
		float  MaxPawnJumpAfter     = 0.0f; // 첫 거부 뒤 화면 캐릭터의 프레임당 튐 (등속 외삽 대비)
		float  MaxCrateSpeedAfter   = 0.0f; // 첫 거부 뒤 클라이언트 상자 최대 속력 (참고 — 스냅샷 보정·캐릭터 밀기 포함)
		float  MaxCrateRiseAfter    = 0.0f; // 첫 거부 뒤 클라이언트 상자가 바닥 위로 뜬 높이
		float  ServerCrateRise      = 0.0f; // 같은 동안 서버 상자가 뜬 높이 (서버 캐릭터가 막을 때 상자가 조금 들린다 — 클라이언트는 이보다 크면 안 된다)
		float  FinalPawnDifference  = 0.0f;
		float  FinalCrateDifference = 0.0f;
	};

	FEmbedResult RunEmbedScenario(int32 LatencyMs, float LossPercent, uint64 Seed)
	{
		RegisterPhysicsTypes();
		RegisterNetworkTypes();
		auto            Hub = std::make_shared<FLoopbackHub>();
		FNetSessionInfo Session;
		FEmbedResult    Result;
		const auto      MakeTransport = [&](uint64 TransportSeed) {
			auto Transport = std::make_unique<FLoopbackTransport>(Hub);
			Transport->SetSimulationSeed(TransportSeed);
			Transport->SetPollIntervalMs(1000.0 / 120.0);
			Transport->SetSimulation(LatencyMs, LossPercent); // 흔들림(기본 배율) = 비신뢰 메시지 재정렬
			return Transport;
		};

		auto Server = std::make_unique<FServerSide>();
		BuildEmbedLevel(Server->Scene, Server->Pawn, Server->Crate);
		Server->World.Init({ &Server->Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Server->Net });
		Server->Net.StartServer(MakeTransport(Seed * 2 + 1), 7782, Session, true);
		Server->Replication.Begin(Server->Scene, Server->Net);
		Server->Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
			Server->Scene.GetRegistry().Get<FReplicatedComponent>(Server->Pawn).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
			Server->Replication.OnPlayerJoined(Player.Connection);
		};
		Server->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Server->World.HandleNetMessage(Connection, Message); };
		Server->World.BeginPlay(Server->Scene, ENetMode::DedicatedServer);

		auto Client = std::make_unique<FClientSide>();
		BuildEmbedLevel(Client->Scene, Client->Pawn, Client->Crate);
		Client->World.Init({ &Client->Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Client->Net });
		Client->Replication.Begin(Client->Scene);
		Client->Replication.SetTransformFilter([&](FEntity Entity) { return !Client->World.IsPredicted(Entity); });
		Client->World.SetReplicationClient(&Client->Replication);
		Client->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) {
			if (!Client->Replication.HandleMessage(Message))
			{
				Client->World.HandleNetMessage(Connection, Message);
			}
		};
		Client->World.BeginPlay(Client->Scene, ENetMode::Client);
		Client->Net.StartClient(MakeTransport(Seed * 2 + 2), "127.0.0.1:7782", Session);

		FInput     ClientInput;
		int32      Frame = 0;
		bool       bLaunched = false, bTeleported = false;
		FVector3   PawnPrevious, PawnCurrent, CratePrevious;
		int32      PawnSamples = 0;
		const auto Pump        = [&](int32 Rounds) {
			for (int32 Round = 0; Round < Rounds; ++Round, ++Frame)
			{
				Client->Net.Update(Step);
				Client->Replication.Update(Step);
				Client->World.TickGameplay(Step, &ClientInput);
				Client->Net.Update(Step);
				Server->Net.Update(Step);
				Server->World.TickGameplay(Step, nullptr);
				// 서버 게임 코드: 상자를 왼쪽으로 밀어 보내고, 상자가 TeleportCrateX를 지나면 캐릭터를 LandX 공중으로 옮긴다 (클라이언트는 결과만 받는다)
				if (bLaunched && !bTeleported && Server->Scene.GetTransform(Server->Crate).Position.X < TeleportCrateX)
				{
					FCharacterState2D State              = Server->World.GetCharacters2D().GetState(Server->Pawn);
					State.Position                       = FVector2(LandX, 80.0f + 5.0f + 60.0f);
					State.Velocity                       = FVector2();
					State.bGrounded                      = false;
					Server->World.GetCharacters2D().SetState(Server->Scene, Server->Pawn, State);
					bTeleported                          = true;
				}
				Server->Replication.Tick(Step);
				Server->Net.Update(Step);

				const uint32 Rejects = Client->World.GetReplayOverlapRejectCount2D();
				if (Rejects > 0 && Result.FirstRejectFrame < 0)
				{
					Result.FirstRejectFrame = Frame;
					Result.bPredictedCrate  = Client->World.IsPhysicsSimulatedLocally(Client->Crate);
				}
				const FVector3 Pawn  = Client->Scene.GetTransform(Client->Pawn).Position;
				const FVector3 Crate = Client->Scene.GetTransform(Client->Crate).Position;
				if (Result.FirstRejectFrame >= 0 && Frame > Result.FirstRejectFrame)
				{
					if (PawnSamples >= 2)
					{
						Result.MaxPawnJumpAfter = std::max(Result.MaxPawnJumpAfter, FVector3::Distance(Pawn, PawnCurrent + (PawnCurrent - PawnPrevious)));
					}
					Result.MaxCrateSpeedAfter = std::max(Result.MaxCrateSpeedAfter, FVector3::Distance(Crate, CratePrevious) / Step);
					Result.MaxCrateRiseAfter  = std::max(Result.MaxCrateRiseAfter, Crate.Z - 40.0f);
				}
				if (bTeleported)
				{
					Result.ServerCrateRise = std::max(Result.ServerCrateRise, Server->Scene.GetTransform(Server->Crate).Position.Z - 40.0f);
				}
				PawnPrevious  = PawnCurrent;
				PawnCurrent   = Pawn;
				CratePrevious = Crate;
				++PawnSamples;
			}
		};
		for (int32 Wait = 0; Wait < 120 && Client->Net.GetClientState() != FNetDriver::EClientState::Joined; ++Wait)
		{
			Pump(1);
		}
		Result.bJoined = Client->Net.GetClientState() == FNetDriver::EClientState::Joined;
		Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = static_cast<int32>(Client->Net.GetLocalPlayerId());
		Pump(60); // 착지 + 시각 맞추기
		FPhysics2DBodyMotion Launch;
		Server->World.GetPhysics2D().GetBodyMotion(Server->Crate, Launch);
		Launch.LinearVelocity = FVector2(EmbedCrateSpeed, 0.0f);
		Server->World.GetPhysics2D().SetBodyMotion(Server->Crate, Launch);
		bLaunched = true;
		Pump(300); // 상자 도착 → 순간이동 → 재조정 → 정착
		const FVector3 ServerPawn   = Server->Scene.GetTransform(Server->Pawn).Position;
		Result.bServerOnFloor       = ServerPawn.Z < 70.0f && std::abs(ServerPawn.X - LandX) < 5.0f;
		Result.OverlapRejects       = Client->World.GetReplayOverlapRejectCount2D();
		Result.FinalPawnDifference  = FVector3::Distance(Client->Scene.GetTransform(Client->Pawn).Position, ServerPawn);
		Result.FinalCrateDifference = FVector3::Distance(Client->Scene.GetTransform(Client->Crate).Position, Server->Scene.GetTransform(Server->Crate).Position);

		Client->World.EndPlay();
		Server->World.EndPlay();
		Client->Net.Shutdown();
		Server->Net.Shutdown();
		return Result;
	}
} // namespace

E_TEST(Physics2DPrediction_PushedCrateReactsImmediatelyAndConverges)
{
	FNetworkSettings& Settings  = FProjectSettings::Get().Network;
	Settings.bPhysicsPrediction = true;
	const FPushResult Predicted = RunPushScenario(0, 0.0f);
	const FPushResult Lossy     = RunPushScenario(50, 5.0f); // 편도 50ms + 손실 5% (양쪽)
	Settings.bPhysicsPrediction = false;
	const FPushResult Interpolated = RunPushScenario(0, 0.0f);
	Settings.bPhysicsPrediction    = true;
	for (const FPushResult* Result : { &Predicted, &Lossy, &Interpolated })
	{
		std::printf("  2D 물리 예측 %s: 접촉 %d → 상자 움직임 %d 프레임, 보정 %u, 상자 최대 튐 %.2fcm, 서버와 차이 %.2fcm, 서버 상자 이동 %.1fcm\n",
					Result == &Predicted ? "켬" : (Result == &Lossy ? "켬(지연·손실)" : "끔"), Result->ContactFrame, Result->CrateMoveFrame,
					Result->Corrections, Result->MaxCrateJump, Result->FinalDifference, Result->ServerCrateTravel);
	}

	// 예측: 반경 안에 들어온 상자는 클라이언트에서 동적, 닿은 프레임(또는 다음 프레임)에 움직인다
	for (const FPushResult* Result : { &Predicted, &Lossy })
	{
		E_EXPECT_TRUE(Result->bPredictedNearCrate);
		E_EXPECT_TRUE(Result->bDynamicOnClient);
		E_EXPECT_TRUE(Result->ContactFrame >= 0 && Result->CrateMoveFrame >= 0);
		E_EXPECT_TRUE(Result->CrateMoveFrame - Result->ContactFrame <= 1);
		E_EXPECT_TRUE(Result->ServerCrateTravel > 50.0f); // 서버에서도 밀렸다
		// 서버 권위: 멈춘 뒤 클라이언트 상자는 서버 상자 위치로 수렴, 화면 튐 없음
		E_EXPECT_TRUE(Result->FinalDifference < 2.0f);
		E_EXPECT_TRUE(Result->MaxCrateJump < 5.0f);
		E_EXPECT_EQ(Result->OverlapRejects, 0u);
		// 해제: 키네마틱 보간으로 복귀
		E_EXPECT_TRUE(Result->bReleased);
		E_EXPECT_TRUE(Result->bKinematicAfterRelease);
	}
	// 끔: 키네마틱 보간 — 서버가 밀고 스냅샷 보간 지연(0.1초)이 지나야 움직인다
	E_EXPECT_FALSE(Interpolated.bPredictedNearCrate);
	E_EXPECT_FALSE(Interpolated.bDynamicOnClient);
	E_EXPECT_TRUE(Interpolated.CrateMoveFrame - Interpolated.ContactFrame >= 5);
	// 캐릭터 보정은 예측 쪽이 훨씬 적다 (끄면 과거 위치의 키네마틱 상자에 막혀 ack마다 보정)
	E_EXPECT_TRUE(Predicted.Corrections * 2 <= Interpolated.Corrections);
	E_EXPECT_TRUE(Interpolated.FinalDifference < 2.0f);
}

// 겹침 거부 전체 네트워크 장면 (루프백 서버·클라이언트, 지연·손실·재정렬): 서버가 예측 캐릭터를 미끄러져 오는 상자 앞 공중으로 옮겨 재조정 ②(기록 자리
// 다시 적용)가 지금 자리 상자에 묻히는 상황에서 겹침 거부가 1회 이상 일어나고, 그 뒤 화면 캐릭터가 튀지 않으며(보정은 화면 오프셋으로 흡수) 상자가
// 대리 캡슐에 튕겨 나가지 않고, 캐릭터·상자가 서버와 수렴한다
E_TEST(Physics2DPrediction_ReplayOverlapRejectInNetwork)
{
	FNetworkSettings& Settings  = FProjectSettings::Get().Network;
	Settings.bPhysicsPrediction = true;
	struct FCase
	{
		int32  LatencyMs;
		float  LossPercent;
		uint64 Seed;
	};
	for (const FCase& Case : { FCase{ 80, 0.0f, 1 }, FCase{ 100, 5.0f, 2 }, FCase{ 120, 10.0f, 3 } })
	{
		const FEmbedResult Result = RunEmbedScenario(Case.LatencyMs, Case.LossPercent, Case.Seed);
		std::printf("  2D 겹침 거부 (지연 %dms 손실 %.0f%%): 거부 %u (첫 프레임 %d), 이후 캐릭터 최대 튐 %.2fcm, 상자 최대 속력 %.1fcm/s, 뜸 %.1fcm (서버 %.1fcm), "
		            "서버와 차이 캐릭터 %.2fcm 상자 %.2fcm, 서버 캐릭터 바닥 %d\n",
		            Case.LatencyMs, Case.LossPercent, Result.OverlapRejects, Result.FirstRejectFrame, Result.MaxPawnJumpAfter, Result.MaxCrateSpeedAfter,
		            Result.MaxCrateRiseAfter, Result.ServerCrateRise, Result.FinalPawnDifference, Result.FinalCrateDifference, Result.bServerOnFloor ? 1 : 0);
		E_EXPECT_TRUE(Result.bJoined);
		E_EXPECT_TRUE(Result.bPredictedCrate);
		E_EXPECT_TRUE(Result.bServerOnFloor);
		E_EXPECT_TRUE(Result.OverlapRejects >= 1u);
		E_EXPECT_TRUE(Result.MaxPawnJumpAfter < 25.0f);
		E_EXPECT_TRUE(Result.MaxCrateRiseAfter <= Result.ServerCrateRise + 2.0f); // 대리 캡슐이 묻힌 상자를 튕겨 올리지 않는다
		E_EXPECT_TRUE(Result.FinalPawnDifference < 2.0f);
		E_EXPECT_TRUE(Result.FinalCrateDifference < 2.0f);
	}
}

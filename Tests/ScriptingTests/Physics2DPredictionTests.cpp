// 2D 물리 예측 (World/GameWorldPhysicsPrediction2D.cpp): 루프백 전용 서버 1 + 클라이언트 1, 양쪽 같은 씬 (정적 NetId).
// 클라이언트 2D 캐릭터가 2D 상자를 미는 장면에서 예측을 켠 경우와 끈 경우를 비교하고(3D PhysicsPrediction_* 와 같은 방식),
// 손실·지연(루프백 시뮬레이션)에서도 서버 결과로 수렴하는지 본다
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

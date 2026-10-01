#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Network/ReplicationTypes.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsReflection.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <cmath>
#include <cstdio>
#include <memory>

// 물리 예측 (World/GameWorldPhysicsPrediction.cpp): 루프백 전용 서버 1 + 클라이언트 1, 양쪽 같은 씬 (정적 NetId).
// 클라이언트 캐릭터가 상자를 미는 장면에서 예측을 켠 경우와 끈 경우를 비교한다
namespace
{
	constexpr float Step   = 1.0f / 60.0f;
	constexpr float CrateX = 150.0f;

	void BuildLevel(FScene& Scene, FEntity& OutPawn, FEntity& OutCrate)
	{
		FRegistry&    Registry = Scene.GetRegistry();
		const FEntity Floor    = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position                  = FVector3(0.0f, 0.0f, -10.0f);
		Registry.Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(3000.0f, 3000.0f, 10.0f);
		Registry.Emplace<FRigidBodyComponent>(Floor).MotionType    = static_cast<int32>(EPhysicsMotionType::Static);
		OutPawn = Scene.CreateEntity("Pawn");
		Scene.GetTransform(OutPawn).Position = FVector3(0.0f, 0.0f, 91.0f);
		Registry.Emplace<FCharacterMovementComponent>(OutPawn);
		Registry.Emplace<FReplicatedComponent>(OutPawn);
		OutCrate = Scene.CreateEntity("Crate");
		Scene.GetTransform(OutCrate).Position = FVector3(CrateX, 0.0f, 30.0f);
		Registry.Emplace<FBoxColliderComponent>(OutCrate).HalfExtents = FVector3(30.0f, 30.0f, 30.0f);
		FRigidBodyComponent& Body = Registry.Emplace<FRigidBodyComponent>(OutCrate);
		Body.MotionType           = static_cast<int32>(EPhysicsMotionType::Dynamic);
		Body.Density              = 120.0f;
		Registry.Emplace<FReplicatedComponent>(OutCrate);
		Scene.UpdateTransforms();
	}

	struct FServerSide
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FPhysicsSystem     Physics;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationServer Replication;
		FEntity            Pawn, Crate;
	};

	struct FClientSide
	{
		FScene             Scene;
		FScriptSystem      Scripts;
		FPhysicsSystem     Physics;
		FNetDriver         Net;
		FGameWorld         World;
		FReplicationClient Replication;
		FEntity            Pawn, Crate;
	};

	struct FPushResult
	{
		bool   bPredictedNearCrate   = false; // 다가가기 전에 (반경 안) 예측 대상이 됐다
		bool   bDynamicOnClient      = false;
		int32  ContactFrame          = -1; // 캐릭터가 상자에 닿을 위치에 온 프레임
		int32  CrateMoveFrame        = -1; // 클라이언트 화면에서 상자가 1cm 움직인 프레임
		uint32 Corrections           = 0;
		float  MaxCrateJump          = 0.0f; // 상자 화면 위치의 프레임당 튐 (등속 외삽과의 차이)
		float  FinalDifference       = 0.0f; // 멈춘 뒤 클라이언트 ↔ 서버 상자 위치 차이
		bool   bReleased             = false; // 멀어지고 멈춘 뒤 보간으로 돌아갔다
		bool   bKinematicAfterRelease = false;
	};

	FPushResult RunPushScenario()
	{
		RegisterPhysicsTypes();
		RegisterNetworkTypes();
		auto            Hub = std::make_shared<FLoopbackHub>();
		FNetSessionInfo Session;
		FPushResult     Result;

		auto Server = std::make_unique<FServerSide>();
		BuildLevel(Server->Scene, Server->Pawn, Server->Crate);
		Server->World.Init({ &Server->Scripts, &Server->Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Server->Net });
		Server->Physics.SetInterpolation(false); // 전용 서버와 같게
		Server->Net.StartServer(std::make_unique<FLoopbackTransport>(Hub), 7777, Session, true);
		Server->Replication.Begin(Server->Scene, Server->Net);
		Server->Net.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) {
			Server->Scene.GetRegistry().Get<FReplicatedComponent>(Server->Pawn).OwnerPlayerId = static_cast<int32>(Player.PlayerId);
			Server->Replication.OnPlayerJoined(Player.Connection);
		};
		Server->Net.OnGameMessage = [&](FNetConnectionId Connection, const std::vector<uint8>& Message) { Server->World.HandleNetMessage(Connection, Message); };
		Server->World.BeginPlay(Server->Scene, ENetMode::DedicatedServer);

		auto Client = std::make_unique<FClientSide>();
		BuildLevel(Client->Scene, Client->Pawn, Client->Crate);
		Client->World.Init({ &Client->Scripts, &Client->Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory(), &Client->Net });
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
		Client->Net.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Session);

		FInput   ClientInput;
		int32    Frame = 0;
		FVector3 CratePrevious, CrateCurrent;
		int32    CrateSamples = 0;
		const auto Pump = [&](int32 Rounds, float InputX) {
			for (int32 Round = 0; Round < Rounds; ++Round, ++Frame)
			{
				// 클라이언트 프레임 (런타임 순서: 수신 → 스냅샷 보간 → 게임플레이)
				Client->Net.Update(Step);
				Client->Replication.Update(Step);
				if (InputX != 0.0f)
				{
					Client->Physics.AddMovementInput(Client->Pawn, FVector3(InputX, 0.0f, 0.0f));
				}
				Client->World.TickGameplay(Step, &ClientInput);
				Client->Net.Update(Step);
				// 서버 프레임 (전용 서버 순서: 수신 → 게임플레이 → 복제)
				Server->Net.Update(Step);
				Server->World.TickGameplay(Step, nullptr);
				Server->Replication.Tick(Step);
				Server->Net.Update(Step);

				const FVector3 Crate = Client->Scene.GetTransform(Client->Crate).Position;
				const FVector3 Pawn  = Client->Scene.GetTransform(Client->Pawn).Position;
				if (Result.ContactFrame < 0 && CrateX - Pawn.X < 35.0f + 30.0f + 2.0f)
				{
					Result.ContactFrame = Frame;
				}
				if (Result.CrateMoveFrame < 0 && std::abs(Crate.X - CrateX) > 1.0f)
				{
					Result.CrateMoveFrame = Frame;
				}
				if (CrateSamples >= 2)
				{
					Result.MaxCrateJump = std::max(Result.MaxCrateJump, FVector3::Distance(Crate, CrateCurrent + (CrateCurrent - CratePrevious)));
				}
				CratePrevious = CrateCurrent;
				CrateCurrent  = Crate;
				++CrateSamples;
			}
		};
		Pump(5, 0.0f);
		// 복제 대신: 클라이언트 씬의 소유자도 서버와 같게 (실제로는 ReplicatedComponent 복제로 온다)
		Client->Scene.GetRegistry().Get<FReplicatedComponent>(Client->Pawn).OwnerPlayerId = static_cast<int32>(Client->Net.GetLocalPlayerId());
		Pump(60, 0.0f); // 착지 + 시각 맞추기 (ack + 스냅샷)
		Result.bPredictedNearCrate = Client->World.IsPhysicsSimulatedLocally(Client->Crate);
		Result.bDynamicOnClient    = Client->Physics.IsDynamicBody(Client->Crate);
		const uint32 Before        = Client->World.GetCharacterCorrectionCount();

		Pump(30, 1.0f);  // 0.5초 밀기
		Pump(120, 0.0f); // 멈추고 서버 상태로 수렴
		Result.Corrections     = Client->World.GetCharacterCorrectionCount() - Before;
		Result.FinalDifference = FVector3::Distance(Client->Scene.GetTransform(Client->Crate).Position, Server->Scene.GetTransform(Server->Crate).Position);

		// 멀어지면 (반경 × 1.25 밖, 1초 + 블렌드) 키네마틱 보간으로 돌아간다
		Pump(90, -1.0f);
		Pump(120, 0.0f);
		Result.bReleased              = !Client->World.IsPhysicsPredicted(Client->Crate);
		Result.bKinematicAfterRelease = !Client->Physics.IsDynamicBody(Client->Crate);

		Client->World.EndPlay();
		Server->World.EndPlay();
		Client->Net.Shutdown();
		Server->Net.Shutdown();
		return Result;
	}
} // namespace

E_TEST(PhysicsPrediction_PushedCrateReactsImmediatelyAndConverges)
{
	FNetworkSettings& Settings = FProjectSettings::Get().Network;
	Settings.bPhysicsPrediction = true;
	const FPushResult Predicted = RunPushScenario();
	Settings.bPhysicsPrediction = false;
	const FPushResult Interpolated = RunPushScenario();
	Settings.bPhysicsPrediction = true;
	for (const FPushResult* Result : { &Predicted, &Interpolated })
	{
		std::printf("  물리 예측 %s: 접촉 %d → 상자 움직임 %d 프레임, 보정 %u, 상자 최대 튐 %.2fcm, 서버와 차이 %.2fcm\n", Result == &Predicted ? "켬" : "끔",
		            Result->ContactFrame, Result->CrateMoveFrame, Result->Corrections, Result->MaxCrateJump, Result->FinalDifference);
	}

	// 예측: 반경 안에 들어온 상자는 클라이언트에서 동적, 닿은 프레임(또는 다음 프레임)에 움직인다
	E_EXPECT_TRUE(Predicted.bPredictedNearCrate);
	E_EXPECT_TRUE(Predicted.bDynamicOnClient);
	E_EXPECT_TRUE(Predicted.ContactFrame >= 0 && Predicted.CrateMoveFrame >= 0);
	E_EXPECT_TRUE(Predicted.CrateMoveFrame - Predicted.ContactFrame <= 1);
	// 끔: 키네마틱 보간 — 서버가 밀고 스냅샷 보간 지연(0.1초)이 지나야 움직인다
	E_EXPECT_FALSE(Interpolated.bPredictedNearCrate);
	E_EXPECT_FALSE(Interpolated.bDynamicOnClient);
	E_EXPECT_TRUE(Interpolated.CrateMoveFrame - Interpolated.ContactFrame >= 5);

	// 캐릭터 보정은 예측 쪽이 훨씬 적다 (끄면 과거 위치의 키네마틱 상자에 막혀 ack마다 보정)
	E_EXPECT_TRUE(Predicted.Corrections * 2 <= Interpolated.Corrections);
	// 서버 권위: 멈춘 뒤 클라이언트 상자는 서버 상자 위치로 수렴, 화면 튐 없음
	E_EXPECT_TRUE(Predicted.FinalDifference < 2.0f);
	E_EXPECT_TRUE(Predicted.MaxCrateJump < 5.0f);
	// 해제: 키네마틱 보간으로 복귀
	E_EXPECT_TRUE(Predicted.bReleased);
	E_EXPECT_TRUE(Predicted.bKinematicAfterRelease);
	E_EXPECT_TRUE(Interpolated.FinalDifference < 2.0f);
}

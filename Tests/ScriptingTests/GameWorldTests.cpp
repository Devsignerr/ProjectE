#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LanDiscovery.h"
#include "Network/ReplicationTypes.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsReflection.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Gameplay.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

// 시작/정지 수명, 게임플레이 틱에서 물리 진행, 스크립트 물리 훅 연결(Physics.Raycast, GetMass)
E_TEST(GameWorld_LifecycleAndPhysicsHooks)
{
	FScene        Scene;
	const FEntity Ball = Scene.CreateEntity("Ball");
	Scene.GetTransform(Ball).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Ball).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ball).Mass       = 3.0f;
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory() });

	// 시작 전 게임플레이 틱은 아무것도 하지 않는다
	World.TickGameplay(1.0f / 60.0f, nullptr);
	E_EXPECT_FALSE(Physics.IsActive());
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Z, 500.0f, 1.0e-4f);

	World.BeginPlay(Scene);
	E_EXPECT_TRUE(World.IsPlaying());
	E_EXPECT_TRUE(Physics.IsActive());
	E_EXPECT_TRUE(Scripts.IsPlaying());
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
	}
	E_EXPECT_TRUE(Scene.GetTransform(Ball).Position.Z < 450.0f); // 0.5초 낙하 ≈ 122cm

	E_EXPECT_TRUE(Scripts.RunString(R"(
local Hit = Physics.Raycast(Vector3(0, 0, 2000), Vector3(0, 0, -1), 5000)
assert(Hit and Hit.entity:GetName() == 'Ball')
assert(math.abs(Hit.entity:GetMass() - 3) < 0.001)
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	World.EndPlay();
	E_EXPECT_FALSE(World.IsPlaying());
	E_EXPECT_FALSE(Physics.IsActive());
	E_EXPECT_FALSE(Scripts.IsPlaying());
}

// 클라이언트 역할: 게임 모듈은 돌지 않고(스크립트는 ClientOnly/Both만), 복제 엔티티(NetId)의 동적 바디는 키네마틱으로 트랜스폼을 따른다.
// 복제되지 않은 로컬 동적 바디는 그대로 시뮬레이션된다
E_TEST(GameWorld_ClientRoleMakesReplicatedBodiesKinematic)
{
	FScene        Scene;
	const FEntity Replicated = Scene.CreateEntity("Replicated");
	Scene.GetTransform(Replicated).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Replicated).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Replicated);
	Scene.GetRegistry().Emplace<FNetIdComponent>(Replicated).NetId = 1;
	const FEntity Local = Scene.CreateEntity("Local");
	Scene.GetTransform(Local).Position = FVector3(300.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Local).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Local);
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.BeginPlay(Scene, ENetMode::Client);
	E_EXPECT_TRUE(World.GetRole() == EWorldRole::Client);
	E_EXPECT_TRUE(Physics.IsActive());
	E_EXPECT_TRUE(Scripts.IsPlaying()); // ClientOnly/Both 스크립트용
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
	}
	E_EXPECT_NEAR(Scene.GetTransform(Replicated).Position.Z, 500.0f, 1.0e-3f); // 중력 없이 그대로
	E_EXPECT_TRUE(Scene.GetTransform(Local).Position.Z < 450.0f);             // 떨어진다

	// 복제 트랜스폼을 옮기면 키네마틱 바디가 따라간다 (속도도 움직임에서 계산된다)
	Scene.GetTransform(Replicated).Position = FVector3(0.0f, 100.0f, 500.0f);
	Scene.UpdateTransforms();
	World.TickGameplay(1.0f / 60.0f, nullptr);
	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector3(0.0f, 100.0f, 2000.0f), FVector3(0.0f, 0.0f, -1.0f), 5000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Replicated);
	E_EXPECT_TRUE(Physics.GetVelocity(Replicated).Y > 1000.0f); // 한 프레임에 100cm → 약 6000cm/s
	World.EndPlay();
	E_EXPECT_FALSE(Physics.IsActive());
}

namespace
{
	// 스크립트가 돌았는지 표시: 시작하면 자기 위치 X를 1로 바꾼다
	std::filesystem::path WriteMarkerScript()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEGameWorldTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/Marker.lua", std::ios::binary | std::ios::trunc);
		File << "local M = {}\nfunction M:OnStart() self.entity:SetPosition(Vector3(1, 0, 0)) end\nreturn M\n";
		return Directory;
	}

	FEntity AddMarker(FScene& Scene, const char* Name, EScriptExecution Location)
	{
		const FEntity    Entity    = Scene.CreateEntity(Name);
		FScriptComponent& Component = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
		Component.ScriptAsset       = "Scripts/Marker.lua";
		Component.ExecutionLocation = static_cast<int32>(Location);
		return Entity;
	}
} // namespace

// ExecutionLocation 필터: Standalone/리슨 = 전부, 전용 서버 = ServerOnly/Both, 클라이언트 = ClientOnly/Both
E_TEST(GameWorld_ScriptExecutionLocationFilter)
{
	const std::filesystem::path Content = WriteMarkerScript();
	const auto RanScripts = [&](ENetMode Mode) {
		FScene        Scene;
		const FEntity Server = AddMarker(Scene, "Server", EScriptExecution::ServerOnly);
		const FEntity Client = AddMarker(Scene, "Client", EScriptExecution::ClientOnly);
		const FEntity Both   = AddMarker(Scene, "Both", EScriptExecution::Both);
		Scene.UpdateTransforms();
		FScriptSystem Scripts;
		FGameWorld    World;
		World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
		World.BeginPlay(Scene, Mode);
		World.TickGameplay(1.0f / 60.0f, nullptr);
		const auto Ran = [&](FEntity Entity) { return Scene.GetTransform(Entity).Position.X == 1.0f; };
		std::string Result = std::string(Ran(Server) ? "S" : "") + (Ran(Client) ? "C" : "") + (Ran(Both) ? "B" : "");
		World.EndPlay();
		return Result;
	};
	E_EXPECT_TRUE(RanScripts(ENetMode::Standalone) == "SCB");
	E_EXPECT_TRUE(RanScripts(ENetMode::ListenServer) == "SCB");
	E_EXPECT_TRUE(RanScripts(ENetMode::DedicatedServer) == "SB");
	E_EXPECT_TRUE(RanScripts(ENetMode::Client) == "CB");
}

// Lua Net 테이블과 소유권: 서버 소유(-1)는 서버에서 조종 권한, 플레이어 소유는 그 플레이어의 기계에서
E_TEST(GameWorld_LuaNetApiAndOwnership)
{
	RegisterNetworkTypes();
	FScene        Scene;
	const FEntity ServerOwned = Scene.CreateEntity("ServerOwned");
	Scene.GetRegistry().Emplace<FReplicatedComponent>(ServerOwned);
	const FEntity Pawn = Scene.CreateEntity("Pawn");
	Scene.GetRegistry().Emplace<FReplicatedComponent>(Pawn).OwnerPlayerId = 0;
	const FEntity Weapon = Scene.CreateEntity("Weapon"); // 복제 표시 없음 → 가장 가까운 복제 조상(Pawn)의 소유자
	Scene.SetParent(Weapon, Pawn);
	const FEntity Other = Scene.CreateEntity("Other");
	Scene.GetRegistry().Emplace<FReplicatedComponent>(Other).OwnerPlayerId = 7;
	Scene.UpdateTransforms();

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory() });

	World.BeginPlay(Scene, ENetMode::Standalone);
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(Net.IsServer() and Net.IsClient() and Net.GetMode() == 'Standalone' and Net.GetLocalPlayerId() == 0)
assert(Scene.Find('ServerOwned'):GetOwner() == -1 and Scene.Find('ServerOwned'):IsLocallyOwned())
assert(Scene.Find('Weapon'):GetOwner() == 0 and Scene.Find('Weapon'):IsLocallyOwned())
assert(Scene.Find('Other'):GetOwner() == 7 and not Scene.Find('Other'):IsLocallyOwned())
)"));
	World.EndPlay();

	World.BeginPlay(Scene, ENetMode::DedicatedServer);
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(Net.IsServer() and not Net.IsClient() and Net.GetMode() == 'DedicatedServer' and Net.GetLocalPlayerId() == -1)
assert(Scene.Find('ServerOwned'):IsLocallyOwned() and not Scene.Find('Pawn'):IsLocallyOwned()) -- 전용 서버에는 플레이어 0이 없다
)"));
	World.EndPlay();

	World.BeginPlay(Scene, ENetMode::Client);
	E_EXPECT_TRUE(Scripts.RunString(R"(
assert(not Net.IsServer() and Net.IsClient() and Net.GetMode() == 'Client')
assert(not Scene.Find('ServerOwned'):IsLocallyOwned()) -- 서버 소유는 클라이언트에서 권한 없음
)"));
	World.EndPlay();
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
}

// Lua 세션 API: FindSessions/GetSessions(LAN), Host/Connect/Disconnect는 요청만 쌓고 앱이 ConsumeSessionRequest로 꺼낸다
E_TEST(GameWorld_LuaSessionApi)
{
	constexpr uint16 DiscoveryPort = 27796;
	FLanDiscovery    LanHost;
	FLanHostInfo     Info;
	Info.Name                = "로비 테스트";
	Info.Session.ProjectName = FPaths::GetProjectName(); // 테스트는 기본 예제 프로젝트(Sample)로 실행된다
	Info.GamePort            = 27797;
	const bool bSockets      = FTestRegistry::AllowRealSockets(); // LAN 검색은 실제 UDP 소켓 — 없으면 세션 요청 부분만
	if (bSockets)
	{
		E_EXPECT_TRUE(LanHost.StartHost(Info, DiscoveryPort));
	}
	else
	{
		FTestRegistry::ReportSkipped("GameWorld_LuaSessionApi (LAN 검색 부분)", "실제 소켓 — E_TEST_SOCKETS=1(-SocketTests)일 때만");
	}

	FScene        Scene;
	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.SetLanDiscoveryPort(DiscoveryPort);
	World.BeginPlay(Scene);

	if (bSockets)
	{
		E_EXPECT_TRUE(Scripts.RunString("assert(Net.GetState() == 'Standalone'); Net.FindSessions()"));
		for (int32 Frame = 0; Frame < 60; ++Frame)
		{
			LanHost.Update();
			World.TickGameplay(1.0f / 60.0f, nullptr);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		E_EXPECT_TRUE(Scripts.RunString(R"(
local Sessions = Net.GetSessions()
assert(#Sessions == 1 and Sessions[1].name == '로비 테스트' and Sessions[1].address:sub(-6) == ':27797')
Net.Connect(Sessions[1].address)
)"));
	}
	else
	{
		E_EXPECT_TRUE(Scripts.RunString("assert(Net.GetState() == 'Standalone'); Net.Connect('127.0.0.1:27797')"));
	}
	std::optional<FNetSessionRequest> Request = World.ConsumeSessionRequest();
	E_EXPECT_TRUE(Request.has_value() && Request->Type == FNetSessionRequest::EType::Connect && Request->Address.ends_with(":27797"));
	E_EXPECT_FALSE(World.ConsumeSessionRequest().has_value()); // 한 번만 꺼내진다

	E_EXPECT_TRUE(Scripts.RunString("Net.Host(9000)"));
	Request = World.ConsumeSessionRequest();
	E_EXPECT_TRUE(Request.has_value() && Request->Type == FNetSessionRequest::EType::Host && Request->Port == 9000);
	E_EXPECT_TRUE(Scripts.RunString("Net.Disconnect()"));
	Request = World.ConsumeSessionRequest();
	E_EXPECT_TRUE(Request.has_value() && Request->Type == FNetSessionRequest::EType::Disconnect);

	// 플레이 중 Standalone → 리슨 서버 전환 (스크립트는 그대로)
	World.SetNetMode(ENetMode::ListenServer);
	E_EXPECT_TRUE(Scripts.RunString("assert(Net.GetMode() == 'ListenServer' and Net.IsServer() and Net.IsClient())"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 샘플 플레이어 캐릭터(Prefabs/Player.eprefab + Scripts/PlayerCharacter.lua + CharacterMovementComponent):
// 로컬 플레이어(소유자 0 = Standalone의 로컬 플레이어)의 입력 → 엔진 캐릭터 이동/점프/몸 방향
E_TEST(PlayerCharacter_MovesJumpsAndFaces)
{
	RegisterPhysicsTypes();
	RegisterNetworkTypes();
	const std::filesystem::path Content = FPaths::GetProjectContentDirectory();
	FPrefabLibrary::Get().SetContentDirectory(Content);

	FScene        Scene;
	const FEntity Ground = Scene.CreateEntity("Ground");
	Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Ground).HalfExtents = FVector3(3000.0f, 3000.0f, 10.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ground).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);
	std::string   Error;
	const FEntity Player = FPrefabLibrary::Get().Instantiate(Scene, "Prefabs/Player.eprefab", NullEntity, &Error);
	E_EXPECT_TRUE(Player.IsValid());
	if (!Player.IsValid())
	{
		return;
	}
	Scene.GetRegistry().Get<FReplicatedComponent>(Player).OwnerPlayerId = 0; // 로컬 플레이어 소유 (스폰너가 하는 일)
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, Content });
	World.BeginPlay(Scene); // Standalone: 로컬 플레이어 0

	FInput     Input;
	const auto Run = [&](float Seconds) {
		for (int32 Frame = 0; Frame < static_cast<int32>(Seconds * 60.0f); ++Frame)
		{
			Input.UpdateActions(FProjectSettings::Get().Input.GetEffectiveMapping(), 1.0f / 60.0f); // 앱 루프와 같이 (예제 Config/Input.json)
			World.TickGameplay(1.0f / 60.0f, &Input);
			Input.EndFrame();
		}
	};
	Run(1.0f); // 착지
	E_EXPECT_TRUE(Physics.HasCharacter(Player));
	const FVector3 Start = Scene.GetTransform(Player).Position;
	E_EXPECT_NEAR(Start.Z, 90.0f, 3.0f); // 캡슐 중심 = 반지름 35 + 원기둥 절반 55

	// W: 시점 방향(yaw 0 = +X)으로 450cm/s
	FInput::FKeyBits Keys;
	Keys[static_cast<size_t>(EKey::W)] = true;
	Input.SetState(Keys, {}, 0, 0, 0.0f);
	Run(1.0f);
	const FVector3 Walked = Scene.GetTransform(Player).Position;
	E_EXPECT_NEAR(Walked.X - Start.X, 450.0f, 20.0f);
	E_EXPECT_NEAR(Walked.Y, Start.Y, 1.0f);
	// 애니메이션 그래프 파라미터 (Body/Mesh의 AnimGraphComponent, 캐릭터 이동 자동 공급) — 캐릭터 루트에서 읽는다
	const auto AnimParam = [&](const char* Name) { return FAnimationSystem::GetAnimParam(Scene, Player, Name).value_or(-1.0f); };
	E_EXPECT_NEAR(AnimParam("Speed"), 450.0f, 5.0f);
	E_EXPECT_NEAR(AnimParam("Grounded"), 1.0f, 1.0e-4f);

	// 시점을 오른쪽 90도로 돌리면 W가 +Y, 몸(캐릭터 루트)도 그쪽을 본다
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Player'):GetScript().Yaw = 90"));
	Run(0.5f);
	E_EXPECT_TRUE(Scene.GetTransform(Player).Position.Y - Walked.Y > 150.0f);
	E_EXPECT_EQUALS(Scene.GetTransform(Player).Rotation.GetForwardVector(), FVector3::RightVector, 1.0e-3f);

	// 스페이스: 바닥에서 점프 (눌린 프레임) → 떠오른다 → 다시 착지
	Keys.reset();
	Keys[static_cast<size_t>(EKey::Space)] = true;
	Input.SetState(Keys, {}, 0, 0, 0.0f);
	Run(0.25f);
	E_EXPECT_TRUE(Scene.GetTransform(Player).Position.Z > Start.Z + 60.0f);
	E_EXPECT_NEAR(AnimParam("Grounded"), 0.0f, 1.0e-4f);
	E_EXPECT_TRUE(AnimParam("VerticalSpeed") > 50.0f);
	Keys.reset();
	Input.SetState(Keys, {}, 0, 0, 0.0f);
	Run(1.5f);
	E_EXPECT_NEAR(Scene.GetTransform(Player).Position.Z, 90.0f, 3.0f);
	E_EXPECT_NEAR(AnimParam("Grounded"), 1.0f, 1.0e-4f);
	E_EXPECT_NEAR(AnimParam("Speed"), 0.0f, 1.0f);

	// 게임패드 (가짜 패드): 왼쪽 스틱 위 = 앞(+Y, 시점 90도), 반만 기울이면 절반 속도, A = 점프, 오른쪽 스틱 = 시점
	FGamepadState Pad;
	Pad.bConnected = true;
	Pad.LeftY      = 1.0f;
	Input.SetGamepadState(Pad);
	const FVector3 PadStart = Scene.GetTransform(Player).Position;
	Run(1.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Player).Position.Y - PadStart.Y, 450.0f, 25.0f);
	Pad.LeftY = 0.625f; // 데드존 0.25 → (0.625-0.25)/0.75 = 0.5
	Input.SetGamepadState(Pad);
	const FVector3 HalfStart = Scene.GetTransform(Player).Position;
	Run(1.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Player).Position.Y - HalfStart.Y, 225.0f, 25.0f);
	Pad.LeftY = 0.0f;
	Pad.SetButton(EGamepadButton::A, true);
	Input.SetGamepadState(Pad);
	Run(0.25f);
	E_EXPECT_TRUE(Scene.GetTransform(Player).Position.Z > Start.Z + 60.0f);
	Pad.SetButton(EGamepadButton::A, false);
	Pad.RightX = 1.0f; // 1500카운트/초 × 0.12도 = 180도/초
	Input.SetGamepadState(Pad);
	Run(0.5f);
	E_EXPECT_TRUE(Scripts.RunString("local Yaw = Scene.Find('Player'):GetScript().Yaw; assert(math.abs(Yaw - 180) < 5, 'yaw ' .. Yaw)")); // 90 + 0.5초 × 180

	// 체력 (Phase 25 통합): 죽으면 입력이 막히고 몸(Body)이 숨는다 → 리스폰(기본 3초, PlayerStart가 없으면 제자리)에서 다시 보인다
	Pad.RightX = 0.0f;
	Input.SetGamepadState(Pad);
	Run(1.0f); // 착지
	FEntity Body;
	for (const FEntity Child : Scene.GetChildren(Player))
	{
		if (Scene.GetRegistry().Get<FNameComponent>(Child).Name == "Body")
		{
			Body = Child;
		}
	}
	E_EXPECT_TRUE(Gameplay::ApplyDamage(Scene, Player, 1000.0f, NullEntity) > 0.0f);
	Run(0.1f);
	E_EXPECT_TRUE(Gameplay::IsDead(Scene, Player));
	E_EXPECT_TRUE(Scene.GetTransform(Body).Scale.X < 0.01f);
	Pad.LeftY = 1.0f;
	Input.SetGamepadState(Pad);
	const FVector3 DeadAt = Scene.GetTransform(Player).Position;
	Run(1.0f);
	E_EXPECT_NEAR(FVector3::Distance(Scene.GetTransform(Player).Position, DeadAt), 0.0f, 1.0f);
	Pad.LeftY = 0.0f;
	Input.SetGamepadState(Pad);
	Run(2.5f);
	E_EXPECT_FALSE(Gameplay::IsDead(Scene, Player));
	E_EXPECT_NEAR(Scene.GetTransform(Body).Scale.X, 1.0f, 1.0e-4f);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// OnLateUpdate는 물리·트랜스폼 갱신 뒤에 불린다: 같은 프레임에 OnUpdate가 본 위치보다 물리가 옮긴 뒤의 위치를 본다 (카메라 따라가기)
E_TEST(GameWorld_LateUpdateSeesPhysicsResult)
{
	const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectELateUpdateTests";
	std::filesystem::create_directories(Content / L"Scripts");
	{
		std::ofstream File(Content / L"Scripts/LateReader.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { UpdateZ = 0, LateZ = 0, LateCount = 0 } }
function T:OnUpdate(dt) self.Properties.UpdateZ = self.entity:GetWorldPosition().Z end
function T:OnLateUpdate(dt)
	self.Properties.LateZ = self.entity:GetWorldPosition().Z
	self.Properties.LateCount = self.Properties.LateCount + 1
end
return T
)";
	}
	FScene        Scene;
	const FEntity Ball = Scene.CreateEntity("Ball");
	Scene.GetTransform(Ball).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Ball).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ball);
	Scene.GetRegistry().Emplace<FScriptComponent>(Ball).ScriptAsset = "Scripts/LateReader.lua";
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
	}
	const double UpdateZ = Scripts.GetInstanceProperty(Ball, "UpdateZ").Number;
	const double LateZ   = Scripts.GetInstanceProperty(Ball, "LateZ").Number;
	E_EXPECT_TRUE(LateZ < UpdateZ - 1.0);                                            // 떨어지는 중: 늦은 갱신이 이번 프레임 물리 결과
	E_EXPECT_NEAR(LateZ, static_cast<double>(Scene.GetTransform(Ball).Position.Z), 1.0e-3); // = 프레임 최종 위치
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Ball, "LateCount").Number, 30.0, 1.0e-9);
	World.EndPlay();
}

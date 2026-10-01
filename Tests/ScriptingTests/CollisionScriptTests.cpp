// 물리 알림 → Lua/게임 모듈 전달 (World/GameWorldPhysicsEvents.cpp): 스크립트 엔티티는 자동 보고, 역할 규칙(클라이언트는 복제 엔티티 제외)
#include "Core/Testing/TestFramework.h"
#include "Network/ReplicationTypes.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	std::filesystem::path WriteCollisionRecorder()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectECollisionScriptTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/CollisionRecorder.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Log = "", NormalZ = 0.0, Impulse = 0.0, PointZ = 999.0 } }
local function Name(e) return e and e:GetName() or "nil" end
function T:OnCollisionBegin(other, info)
	self.Properties.Log = self.Properties.Log .. "B:" .. Name(other) .. ";"
	self.Properties.NormalZ = info.Normal.Z
	self.Properties.Impulse = info.Impulse
	self.Properties.PointZ  = info.Point.Z
	assert(info.Speed > 0)
end
function T:OnCollisionEnd(other) self.Properties.Log = self.Properties.Log .. "E:" .. Name(other) .. ";" end
function T:OnTriggerEnter(other) self.Properties.Log = self.Properties.Log .. "TE:" .. Name(other) .. ";" end
function T:OnTriggerExit(other) self.Properties.Log = self.Properties.Log .. "TX:" .. Name(other) .. ";" end
function T:OnJointBreak(other, force) self.Properties.Log = self.Properties.Log .. "J:" .. Name(other) .. ";"; self.Properties.Impulse = force end
return T
)";
		return Directory;
	}

	FEntity AddScripted(FScene& Scene, const char* Name, int32 Location = static_cast<int32>(EScriptExecution::Both))
	{
		const FEntity     Entity = Scene.CreateEntity(Name);
		FScriptComponent& Script = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
		Script.ScriptAsset       = "Scripts/CollisionRecorder.lua";
		Script.ExecutionLocation = Location;
		return Entity;
	}

	struct FCollisionModule final : IGameModule
	{
		FEntity Watched;
		int32   Begins = 0, Ends = 0, Enters = 0, Exits = 0;
		bool WantsCollisionEvents(const FScene&, FEntity Entity) const override { return Entity == Watched; }
		void OnCollisionBegin(FScene&, const FCollisionEvent& Event) override { Begins += Event.Self == Watched ? 1 : 0; }
		void OnCollisionEnd(FScene&, const FCollisionEvent& Event) override { Ends += Event.Self == Watched ? 1 : 0; }
		void OnTriggerEnter(FScene&, const FCollisionEvent&) override { ++Enters; }
		void OnTriggerExit(FScene&, const FCollisionEvent&) override { ++Exits; }
	};

	std::string LogOf(FScriptSystem& Scripts, FEntity Entity) { return Scripts.GetInstanceProperty(Entity, "Log").String; }
} // namespace

// Standalone: 스크립트가 붙은 공(ReportContacts 끔)이 바닥에 닿으면 OnCollisionBegin(other, info), 트리거 통과는 OnTriggerEnter/Exit(양쪽).
// 게임 모듈은 WantsCollisionEvents로 고른 엔티티(스크립트 없음)의 이벤트를 받는다
E_TEST(CollisionScript_LuaAndGameModuleEvents)
{
	const std::filesystem::path Content = WriteCollisionRecorder();
	FScene                      Scene;
	const FEntity Floor = AddScripted(Scene, "Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);

	const FEntity Ball = AddScripted(Scene, "Ball");
	Scene.GetTransform(Ball).Position = FVector3(0.0f, 0.0f, 300.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Ball).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ball).Mass       = 2.0f;

	const FEntity Zone = AddScripted(Scene, "Zone");
	Scene.GetTransform(Zone).Position = FVector3(0.0f, 0.0f, 150.0f);
	FBoxColliderComponent& ZoneBox    = Scene.GetRegistry().Emplace<FBoxColliderComponent>(Zone);
	ZoneBox.HalfExtents               = FVector3(100.0f, 100.0f, 20.0f);
	ZoneBox.bIsTrigger                = true;

	// 스크립트 없는 상자: 게임 모듈만 원한다
	const FEntity Crate = Scene.CreateEntity("Crate");
	Scene.GetTransform(Crate).Position = FVector3(400.0f, 0.0f, 100.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Crate).HalfExtents = FVector3(20.0f, 20.0f, 20.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Crate);
	Scene.UpdateTransforms();

	FScriptSystem    Scripts;
	FPhysicsSystem   Physics;
	FCollisionModule Module;
	Module.Watched = Crate;
	FGameModuleHost Host;
	Host.Attach(Module, "CollisionTestModule");
	FGameWorld World;
	World.Init({ &Scripts, &Physics, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 90; ++Frame) // 1.5초: 트리거 통과 → 바닥
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Ball) == "TE:Zone;TX:Zone;B:Floor;");
	E_EXPECT_TRUE(LogOf(Scripts, Zone) == "TE:Ball;TX:Ball;");
	E_EXPECT_TRUE(LogOf(Scripts, Floor).find("B:Ball;") != std::string::npos);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Ball, "NormalZ").Number, 1.0, 0.01);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Ball, "PointZ").Number, 0.0, 3.0);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Ball, "Impulse").Number > 500.0); // 2kg × 약 700cm/s
	E_EXPECT_EQ(Module.Begins, 1); // 상자 ↔ 바닥 (상자 쪽)
	E_EXPECT_EQ(Module.Enters, 2); // 트리거 쌍 양쪽 (모듈은 모든 보고 이벤트를 받는다)
	E_EXPECT_EQ(Module.Exits, 2);

	// 공을 지우면 바닥 스크립트가 끝을 받는다 (상대 nil — 지연 파괴 뒤 다음 물리 갱신)
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Ball'):Destroy()"));
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Floor).find("E:nil;") != std::string::npos);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
	Host.Unload();
}

// 클라이언트: 복제 엔티티(NetId)는 서버가 판정하므로 이벤트를 받지 않고, 클라이언트에만 있는 로컬 엔티티 스크립트는 받는다
E_TEST(CollisionScript_ClientSkipsReplicatedEntities)
{
	const std::filesystem::path Content = WriteCollisionRecorder();
	FScene                      Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);

	const FEntity Local = AddScripted(Scene, "Local", static_cast<int32>(EScriptExecution::ClientOnly));
	Scene.GetTransform(Local).Position = FVector3(0.0f, 0.0f, 100.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Local).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Local);

	const FEntity Zone = AddScripted(Scene, "Zone", static_cast<int32>(EScriptExecution::ClientOnly));
	Scene.GetRegistry().Emplace<FNetIdComponent>(Zone).NetId = 7; // 복제 트리거
	Scene.GetTransform(Zone).Position = FVector3(0.0f, 0.0f, 100.0f);
	FBoxColliderComponent& ZoneBox    = Scene.GetRegistry().Emplace<FBoxColliderComponent>(Zone);
	ZoneBox.HalfExtents               = FVector3(100.0f, 100.0f, 100.0f);
	ZoneBox.bIsTrigger                = true;
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, Content });
	World.BeginPlay(Scene, ENetMode::Client);
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Local).find("B:nil;") == std::string::npos);
	E_EXPECT_TRUE(LogOf(Scripts, Local).find("B:Floor;") != std::string::npos); // 로컬 공 ↔ 바닥
	E_EXPECT_TRUE(LogOf(Scripts, Local).find("TE:Zone;") != std::string::npos); // 로컬 쪽은 복제 트리거에 들어온 것을 받는다
	E_EXPECT_TRUE(LogOf(Scripts, Zone).empty());                                 // 복제 트리거 스크립트는 받지 않는다
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 관절 끊어짐 → 관절 엔티티 스크립트 OnJointBreak(other, force) + 게임 모듈 OnJointBreak. 관절 컴포넌트는 리플렉션으로 Lua에서 읽힌다
E_TEST(CollisionScript_JointBreakEvent)
{
	const std::filesystem::path Content = WriteCollisionRecorder();
	FScene                      Scene;
	const FEntity Anchor = Scene.CreateEntity("Anchor");
	Scene.GetTransform(Anchor).Position = FVector3(0.0f, 0.0f, 400.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Anchor).HalfExtents = FVector3(10.0f, 10.0f, 10.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Anchor).MotionType = static_cast<int32>(EPhysicsMotionType::Kinematic);

	const FEntity Sign = AddScripted(Scene, "Sign");
	Scene.GetTransform(Sign).Position = FVector3(0.0f, 0.0f, 300.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Sign).HalfExtents = FVector3(20.0f, 5.0f, 20.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Sign).Mass = 20.0f; // 196N
	FFixedJointComponent& Joint = Scene.GetRegistry().Emplace<FFixedJointComponent>(Sign);
	Joint.Target                = Anchor;
	Joint.BreakForce            = 400.0f;
	Scene.UpdateTransforms();

	struct FBreakModule final : IGameModule
	{
		int32 Breaks = 0;
		void  OnJointBreak(FScene&, const FCollisionEvent&) override { ++Breaks; }
	} Module;
	FGameModuleHost Host;
	Host.Attach(Module, "JointBreakTestModule");
	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Sign).empty()); // 무게(196N)는 버틴다
	E_EXPECT_NEAR(Scene.GetTransform(Sign).Position.Z, 300.0f, 1.0f);
	E_EXPECT_TRUE(Scripts.RunString("local J = Scene.Find('Sign'):GetComponent('FixedJointComponent'); assert(J.BreakForce == 400)"));

	// 아래로 세게 치면 끊어진다
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Sign'):AddImpulse(Vector3(0, 0, -40000))")); // 20kg × 20m/s
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Sign) == "J:Anchor;");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Sign, "Impulse").Number > 400.0);
	E_EXPECT_EQ(Module.Breaks, 1);
	E_EXPECT_TRUE(Physics.IsJointBroken(Sign));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
	Host.Unload();
}

// 2D 물리 → FGameWorld 경로 (World/GameWorld.cpp, GameWorldPhysicsEvents.cpp): 3D와 같은 Lua/게임 모듈 충돌 콜백,
// Lua Physics2D.Raycast/OverlapBox/OverlapCircle, entity:AddImpulse/GetVelocity/GetMass가 2D 강체에 동작
#include "Core/Testing/TestFramework.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DSystem.h"
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

	std::filesystem::path WriteRecorder()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEPhysics2DScriptTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/Recorder2D.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Log = "", NormalZ = 0.0, PointY = 999.0 } }
local function Name(e) return e and e:GetName() or "nil" end
function T:OnCollisionBegin(other, info)
	self.Properties.Log = self.Properties.Log .. "B:" .. Name(other) .. ";"
	self.Properties.NormalZ = info.Normal.Z
	self.Properties.PointY  = info.Point.Y
end
function T:OnCollisionEnd(other) self.Properties.Log = self.Properties.Log .. "E:" .. Name(other) .. ";" end
function T:OnTriggerEnter(other) self.Properties.Log = self.Properties.Log .. "TE:" .. Name(other) .. ";" end
function T:OnTriggerExit(other) self.Properties.Log = self.Properties.Log .. "TX:" .. Name(other) .. ";" end
return T
)";
		return Directory;
	}

	FEntity AddScripted(FScene& Scene, const char* Name)
	{
		const FEntity Entity = Scene.CreateEntity(Name);
		Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Recorder2D.lua";
		return Entity;
	}

	struct FCollisionModule final : IGameModule
	{
		FEntity Watched;
		int32   Begins = 0;
		bool    bHadPhysics2D = false;
		bool WantsCollisionEvents(const FScene&, FEntity Entity) const override { return Entity == Watched; }
		void OnBeginPlay(FScene&) override { bHadPhysics2D = GetPhysics2D() != nullptr; }
		void OnCollisionBegin(FScene&, const FCollisionEvent& Event) override { Begins += Event.Self == Watched ? 1 : 0; }
	};

	std::string LogOf(FScriptSystem& Scripts, FEntity Entity) { return Scripts.GetInstanceProperty(Entity, "Log").String; }
} // namespace

// 2D 충돌·트리거가 3D와 같은 Lua 콜백·게임 모듈 콜백으로 온다 (점 Y = 받는 엔티티 깊이, 법선은 평면 위)
E_TEST(Physics2DScript_CollisionCallbacks)
{
	const std::filesystem::path Content = WriteRecorder();
	FScene                      Scene;
	const FEntity Floor = AddScripted(Scene, "Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(4000.0f, 100.0f);

	const FEntity Ball = AddScripted(Scene, "Ball");
	Scene.GetTransform(Ball).Position = FVector3(0.0f, 70.0f, 300.0f);
	Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Ball).Radius = 25.0f;
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Ball);

	const FEntity Zone = AddScripted(Scene, "Zone");
	Scene.GetTransform(Zone).Position = FVector3(0.0f, 0.0f, 150.0f);
	FBoxCollider2DComponent& ZoneBox  = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Zone);
	ZoneBox.Size                      = FVector2(200.0f, 40.0f);
	ZoneBox.bIsTrigger                = true;

	const FEntity Crate = Scene.CreateEntity("Crate"); // 스크립트 없음: 게임 모듈만 원한다
	Scene.GetTransform(Crate).Position = FVector3(600.0f, 0.0f, 100.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Crate).Size = FVector2(40.0f, 40.0f);
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Crate);
	Scene.UpdateTransforms();

	FScriptSystem    Scripts;
	FCollisionModule Module;
	Module.Watched = Crate;
	FGameModuleHost Host;
	Host.Attach(Module, "Collision2DTestModule");
	FGameWorld World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content }); // 3D 물리 없이도 2D는 돈다
	World.BeginPlay(Scene);
	E_EXPECT_TRUE(World.GetPhysics2D().IsActive());
	E_EXPECT_TRUE(Module.bHadPhysics2D);
	for (int32 Frame = 0; Frame < 90; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Ball) == "TE:Zone;TX:Zone;B:Floor;");
	E_EXPECT_TRUE(LogOf(Scripts, Zone) == "TE:Ball;TX:Ball;");
	E_EXPECT_TRUE(LogOf(Scripts, Floor).find("B:Ball;") != std::string::npos);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Ball, "NormalZ").Number, 1.0, 0.01);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Ball, "PointY").Number, 70.0, 1.0e-3);
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Y, 70.0f, 1.0e-4f); // 깊이 유지
	E_EXPECT_EQ(Module.Begins, 1);

	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Ball'):Destroy()"));
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(LogOf(Scripts, Floor).find("E:nil;") != std::string::npos);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
	E_EXPECT_FALSE(World.GetPhysics2D().IsActive());
	Host.Unload();
}

// Lua: Physics2D 질의 (Vector3/Vector2 둘 다), 엔티티 물리 함수가 2D 강체에 동작
E_TEST(Physics2DScript_LuaQueriesAndForces)
{
	const std::filesystem::path Content = WriteRecorder();
	FScene                      Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(4000.0f, 100.0f);
	const FEntity Puck = Scene.CreateEntity("Puck");
	Scene.GetTransform(Puck).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Puck).Size = FVector2(100.0f, 100.0f);
	FRigidBody2DComponent& Body = Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Puck);
	Body.GravityScale           = 0.0f;
	Body.Mass                   = 4.0f;
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Hit = Physics2D.Raycast(Vector3(0, 33, 1000), Vector3(0, 0, -1), 2000)
assert(Hit and Hit.Entity:GetName() == 'Puck')
assert(math.abs(Hit.Point.Z - 550) < 0.5 and Hit.Point.Y == 33 and math.abs(Hit.Normal.Z - 1) < 1e-3)
assert(math.abs(Hit.Distance - 450) < 0.5 and math.abs(Hit.Fraction - 0.225) < 1e-3)
local Low = Physics2D.Raycast(Vector2(300, 100), Vector2(0, -1), 500)
assert(Low and Low.Entity:GetName() == 'Floor')
assert(Physics2D.Raycast(Vector2(300, 100), Vector2(0, -1), 50) == nil)
assert(not pcall(Physics2D.Raycast, Vector2(0, 0), Vector2(0, -1), 10, {'NoSuchLayer'}))
assert(#Physics2D.OverlapCircle(Vector3(0, 0, 500), 10) == 1)
assert(#Physics2D.OverlapBox(Vector2(0, 0), Vector2(5000, 2000)) == 2)
assert(#Physics2D.OverlapBox(Vector2(200, 300), Vector2(50, 50), 45) == 0)
local Puck = Scene.Find('Puck')
assert(math.abs(Puck:GetMass() - 4) < 1e-3)
Puck:AddImpulse(Vector3(400, 0, 0)) -- 4kg → 100 cm/s
)"));
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local V = Scene.Find('Puck'):GetVelocity()
assert(math.abs(V.X - 100) < 0.5 and V.Y == 0 and math.abs(V.Z) < 1e-3)
)"));
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_NEAR(Scene.GetTransform(Puck).Position.X, 100.0f + 100.0f / 60.0f, 3.0f);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 2D 관절 끊어짐이 3D와 같은 OnJointBreak(other, force)로 오고, Lua Physics2D.BeginDrag/UpdateDrag/EndDrag가 동적 2D 바디를 끈다
E_TEST(Physics2DScript_JointBreakAndDrag)
{
	const std::filesystem::path Content = WriteRecorder();
	{
		std::ofstream File(Content / L"Scripts/JointRecorder2D.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Other = "", Force = 0.0 } }
function T:OnJointBreak(other, force)
	self.Properties.Other = other and other:GetName() or "nil"
	self.Properties.Force = force
end
return T
)";
	}
	FScene        Scene;
	const FEntity Hook = Scene.CreateEntity("Hook");
	Scene.GetTransform(Hook).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Hook).Size = FVector2(20.0f, 20.0f);
	const FEntity Crate = Scene.CreateEntity("Crate");
	Scene.GetTransform(Crate).Position = FVector3(0.0f, 0.0f, 400.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Crate).Size = FVector2(50.0f, 50.0f);
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Crate).Mass = 10.0f;
	FRevoluteJoint2DComponent& Joint = Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Crate);
	Joint.Target     = Hook;
	Joint.Anchor     = FVector2(0.0f, 100.0f);
	Joint.BreakForce = 50.0f;
	Scene.GetRegistry().Emplace<FScriptComponent>(Crate).ScriptAsset = "Scripts/JointRecorder2D.lua";
	const FEntity Puck = Scene.CreateEntity("Puck");
	Scene.GetTransform(Puck).Position = FVector3(1000.0f, 0.0f, 0.0f);
	Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Puck).Radius = 20.0f;
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Puck).GravityScale = 0.0f;
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Puck = Scene.Find('Puck')
assert(not Physics2D.BeginDrag(Scene.Find('Hook'), Vector2(0, 500))) -- 정적
assert(Physics2D.BeginDrag(Puck, Vector3(1000, 0, 0)))
assert(Physics2D.UpdateDrag(Puck, Vector2(1100, 50)))
)"));
	for (int32 Frame = 0; Frame < 90; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_NEAR(Scene.GetTransform(Puck).Position.X, 1100.0f, 10.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Puck).Position.Z, 50.0f, 10.0f);
	E_EXPECT_TRUE(Scripts.RunString("assert(Physics2D.EndDrag(Scene.Find('Puck'))); assert(not Physics2D.EndDrag(Scene.Find('Puck')))"));
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Crate, "Other").String == "Hook");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Crate, "Force").Number > 50.0);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// Lua 2D 관절 실시간 제어: entity:SetJointMotorSpeed/SetJointMaxMotorForce/EnableJointMotor/SetJointLimits/EnableJointLimit/SetJointSpring가
// 컴포넌트 값을 바꾸고 관절을 다시 만들지 않고 반영, GetJointAngle/Translation/Speed로 상태를 읽는다
E_TEST(Physics2DScript_JointControl)
{
	const std::filesystem::path Content = WriteRecorder();
	FScene                      Scene;
	const FEntity               Wheel = Scene.CreateEntity("Wheel");
	Scene.GetTransform(Wheel).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Wheel).Radius = 50.0f;
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Wheel).AngularDamping = 0.0f;
	Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Wheel).MaxMotorTorque = 10000.0f;
	const FEntity Slider = Scene.CreateEntity("Slider");
	Scene.GetTransform(Slider).Position = FVector3(300.0f, 0.0f, 200.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Slider).Size = FVector2(40.0f, 40.0f);
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Slider);
	Scene.GetRegistry().Emplace<FPrismaticJoint2DComponent>(Slider);
	const FEntity Rock = Scene.CreateEntity("Rock");
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Rock);
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Wheel, Slider = Scene.Find('Wheel'), Scene.Find('Slider')
assert(Wheel:EnableJointMotor(true))
assert(Wheel:SetJointMotorSpeed(120))
assert(Wheel:GetComponent('RevoluteJoint2DComponent').MotorSpeed == 120)
assert(Slider:SetJointMaxMotorForce(5000) and Slider:EnableJointMotor(true) and Slider:SetJointMotorSpeed(200))
assert(Slider:SetJointLimits(80, -10)) -- 순서 무관, 한계도 켠다
local Rail = Slider:GetComponent('PrismaticJoint2DComponent')
assert(Rail.Limit == true and Rail.LowerTranslation == -10 and Rail.UpperTranslation == 80)
assert(not Scene.Find('Rock'):SetJointMotorSpeed(10)) -- 관절 없음
assert(Scene.Find('Rock'):GetJointAngle() == 0)
assert(not Wheel:SetJointSpring(2)) -- 회전 관절에는 스프링 필드가 없다
)"));
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Wheel, Slider = Scene.Find('Wheel'), Scene.Find('Slider')
assert(math.abs(Wheel:GetJointSpeed() - 120) < 3, Wheel:GetJointSpeed())
assert(math.abs(Wheel:GetJointAngle() - 118) < 8, Wheel:GetJointAngle())
assert(math.abs(Slider:GetJointTranslation() - 80) < 1.5, Slider:GetJointTranslation())
assert(Slider:EnableJointLimit(false))
assert(Slider:GetComponent('PrismaticJoint2DComponent').Limit == false)
)"));
	World.TickGameplay(Step, nullptr);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("local S = Scene.Find('Slider'); assert(S:GetJointTranslation() > 80.5 and S:GetJointSpeed() > 50, S:GetJointSpeed())"));
	E_EXPECT_EQ(World.GetPhysics2D().GetJointCount(), 2u);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

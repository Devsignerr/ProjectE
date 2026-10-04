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

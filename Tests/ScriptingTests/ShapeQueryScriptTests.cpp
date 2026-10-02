// Lua Physics.Overlap*/ *Cast (FScriptPhysicsHooks, FGameWorld가 연결) + 게임 모듈 GetPhysics() (Phase 41-2)
#include "Core/Testing/TestFramework.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
	std::filesystem::path WriteShapeQueryScript()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEShapeQueryScriptTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/ShapeQuery.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local Q = {}
function Q:OnUpdate(dt)
	if Time.FrameCount < 2 or self.Done then return end -- 바디는 첫 틱 물리 갱신에서 생긴다
	self.Done = true
	self.AllCount = #Physics.OverlapSphere(Vector3(0, 0, 50), 300)
	local Names = {}
	for _, Entity in ipairs(Physics.OverlapSphere(Vector3(0, 0, 50), 300, self.entity)) do Names[#Names + 1] = Entity:GetName() end
	table.sort(Names)
	self.NoSelf = table.concat(Names, ',')
	local Box = Physics.OverlapBox(Vector3(200, 0, 150), Vector3(10, 10, 60), Quat.Identity())
	self.BoxName = #Box == 1 and Box[1]:GetName() or '?'
	self.CapsuleCount = #Physics.OverlapCapsule(Vector3(200, 0, 300), 10, 100) -- 아래 끝 z 190: 상자 위
	local Hit = Physics.SphereCast(Vector3(0, 0, 50), 10, Vector3(1, 0, 0), 1000, self.entity)
	self.CastName, self.CastDistance = Hit.entity:GetName(), Hit.distance
	self.BoxCastDistance = Physics.BoxCast(Vector3(200, 0, 400), Vector3(10, 10, 10), Vector3(0, 0, -1), 1000).distance
	self.CapsuleCastName = Physics.CapsuleCast(Vector3(-300, 0, 400), 10, 20, Vector3(0, 0, -1), 1000).entity:GetName()
	self.Miss = Physics.SphereCast(Vector3(0, 0, 500), 10, Vector3(0, 0, 1), 100) == nil
end
return Q
)";
		return Directory;
	}

	struct FQueryModule final : IGameModule
	{
		uint32 Found         = 0;
		bool   bHadPhysics   = false;
		void   OnUpdate(FScene&, float) override
		{
			if (GetPhysics() != nullptr)
			{
				bHadPhysics = true;
				std::vector<FEntity> Entities;
				Found = GetPhysics()->OverlapSphere(FVector3(200.0f, 0.0f, 50.0f), 10.0f, Entities);
			}
		}
	};

	FEntity AddStaticBox(FScene& Scene, const char* Name, const FVector3& Center, const FVector3& HalfExtents)
	{
		const FEntity Entity = Scene.CreateEntity(Name);
		Scene.GetTransform(Entity).Position = Center;
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Entity).HalfExtents = HalfExtents;
		Scene.GetRegistry().Emplace<FRigidBodyComponent>(Entity).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);
		return Entity;
	}
} // namespace

E_TEST(ShapeQueryScript_LuaAndGameModule)
{
	const std::filesystem::path Content = WriteShapeQueryScript();
	FScene                      Scene;
	AddStaticBox(Scene, "Floor", FVector3(0.0f, 0.0f, -10.0f), FVector3(1000.0f, 1000.0f, 10.0f)); // 윗면 z 0
	AddStaticBox(Scene, "Crate", FVector3(200.0f, 0.0f, 50.0f), FVector3(50.0f, 50.0f, 50.0f));   // x 150~250, z 0~100
	const FEntity Seeker = Scene.CreateEntity("Seeker");
	Scene.GetTransform(Seeker).Position = FVector3(0.0f, 0.0f, 50.0f);
	Scene.GetRegistry().Emplace<FSphereColliderComponent>(Seeker).Radius = 20.0f; // 정적 구 (자기 자신은 ignore로 뺀다)
	Scene.GetRegistry().Emplace<FScriptComponent>(Seeker).ScriptAsset   = "Scripts/ShapeQuery.lua";
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FPhysicsSystem  Physics;
	FQueryModule    Module;
	FGameModuleHost Host;
	Host.Attach(Module, "ShapeQueryTestModule");
	FGameWorld World;
	World.Init({ &Scripts, &Physics, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
	}
	E_EXPECT_TRUE(Scripts.RunString(R"(
local S = Scene.Find('Seeker'):GetScript()
assert(S.Done)
assert(S.AllCount == 3, 'All ' .. tostring(S.AllCount))
assert(S.NoSelf == 'Crate,Floor', S.NoSelf)
assert(S.BoxName == 'Crate' and S.CapsuleCount == 0)
assert(S.CastName == 'Crate' and math.abs(S.CastDistance - 140) < 0.5, tostring(S.CastDistance))
assert(math.abs(S.BoxCastDistance - 290) < 0.5, tostring(S.BoxCastDistance))
assert(S.CapsuleCastName == 'Floor' and S.Miss)
)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_TRUE(Module.bHadPhysics);
	E_EXPECT_EQ(Module.Found, 1u);
	World.EndPlay();
	E_EXPECT_TRUE(Module.GetPhysics() == nullptr); // EndPlay 뒤 비운다
	Host.Unload();
}

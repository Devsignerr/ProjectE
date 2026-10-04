// 2D 캐릭터 이동기 ↔ Lua (World/GameWorld.cpp 훅, World/GameWorldCharacter2D.cpp 이벤트): entity:AddMovementInput/Jump/StopJumping/Dash/
// DropDown/IsGrounded/GetMovementVelocity/GetJumpsRemaining/GetDashesRemaining/IsDashing이 2D 이동기에 동작하고,
// OnJumped(n)/OnLanded()/OnDashStarted()가 충돌 알림 단계에서 오며, 게임 모듈은 GetCharacters2D/OnCharacter2DEvent를 받는다
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/PhysicsReflection.h"
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

	std::filesystem::path WriteController()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectECharacter2DScriptTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/Runner2D.lua", std::ios::binary | std::ios::trunc);
		// 프레임 시간표: 30~59 달리기, 70 점프, 74 뗌(가변 점프), 80 2단 점프, 150 대시(왼쪽), 200 점프(남은 수 확인)
		File << R"(
local T = { Properties = { Log = "", Frame = 0, Grounded = false, VelX = 0.0, JumpsLeft = -1, DashesLeft = -1, DashingSeen = false, AirJumpsLeft = -1 } }
function T:OnUpdate(dt)
	local P = self.Properties
	P.Frame = P.Frame + 1
	local e = self.entity
	if P.Frame >= 30 and P.Frame < 60 then e:AddMovementInput(Vector3(1, 0, 0)) end
	if P.Frame == 60 then P.VelX = e:GetMovementVelocity().X; P.Grounded = e:IsGrounded(); P.JumpsLeft = e:GetJumpsRemaining(); P.DashesLeft = e:GetDashesRemaining() end
	if P.Frame == 70 then e:Jump() end
	if P.Frame == 74 then e:StopJumping() end
	if P.Frame == 78 then P.AirJumpsLeft = e:GetJumpsRemaining() end
	if P.Frame == 80 then e:Jump() end
	if P.Frame == 150 then e:Dash(Vector3(-1, 0, 0)) end
	if P.Frame == 152 then P.DashingSeen = e:IsDashing() end
end
function T:OnJumped(n) self.Properties.Log = self.Properties.Log .. "J" .. n .. ";" end
function T:OnLanded() self.Properties.Log = self.Properties.Log .. "L;" end
function T:OnDashStarted() self.Properties.Log = self.Properties.Log .. "D;" end
return T
)";
		return Directory;
	}

	struct FCharacterModule final : IGameModule
	{
		bool  bHadCharacters = false;
		int32 Jumps = 0, Landings = 0, Dashes = 0;
		void  OnBeginPlay(FScene&) override { bHadCharacters = GetCharacters2D() != nullptr; }
		void  OnCharacter2DEvent(FScene&, FEntity, const FCharacterMove2DEvents& Events) override
		{
			Jumps += Events.bJumped ? 1 : 0;
			Landings += Events.bLanded ? 1 : 0;
			Dashes += Events.bDashStarted ? 1 : 0;
		}
	};
} // namespace

E_TEST(Character2DScript_LuaApiAndCallbacks)
{
	RegisterPhysicsTypes();
	const std::filesystem::path Content = WriteController();
	FScene                      Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(10000.0f, 100.0f);
	const FEntity Hero = Scene.CreateEntity("Hero");
	Scene.GetTransform(Hero).Position = FVector3(0.0f, 0.0f, 70.0f);
	Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Hero);
	Scene.GetRegistry().Emplace<FScriptComponent>(Hero).ScriptAsset = "Scripts/Runner2D.lua";
	Scene.UpdateTransforms();

	FScriptSystem   Scripts;
	FCharacterModule Module;
	FGameModuleHost Host;
	Host.Attach(Module, "Character2DTestModule");
	FGameWorld World;
	World.Init({ &Scripts, nullptr, &Host, nullptr, Content }); // 3D 물리 없이
	World.BeginPlay(Scene);
	E_EXPECT_TRUE(Module.bHadCharacters);
	for (int32 Frame = 0; Frame < 240; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	const auto Number = [&](const char* Name) { return Scripts.GetInstanceProperty(Hero, Name).Number; };
	E_EXPECT_NEAR(Number("VelX"), 600.0, 1.0);   // 30프레임 달리기 → 최대 속력
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Grounded").bBool);
	E_EXPECT_NEAR(Number("JumpsLeft"), 2.0, 0.0);
	E_EXPECT_NEAR(Number("DashesLeft"), 1.0, 0.0);
	E_EXPECT_NEAR(Number("AirJumpsLeft"), 1.0, 0.0); // 첫 점프 뒤 1번 남음
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "DashingSeen").bBool);
	// 처음 내려앉기(L) → 점프 1 → 2단 점프 → 착지 → 대시
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Hero, "Log").String == "L;J1;J2;L;D;");
	E_EXPECT_EQ(Module.Jumps, 2);
	E_EXPECT_EQ(Module.Landings, 2);
	E_EXPECT_EQ(Module.Dashes, 1);
	E_EXPECT_TRUE(World.GetCharacters2D().IsGrounded(Hero));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
	Host.Unload();
}

// 밟기 (CharacterCollision Block): 위에서 떨어진 캐릭터 OnStomped(other), 밟힌 캐릭터 OnStompedBy(other) — 착지와 같은 단계
E_TEST(Character2DScript_StompCallbacks)
{
	RegisterPhysicsTypes();
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectECharacter2DStompTests";
	std::filesystem::create_directories(Directory / L"Scripts");
	{
		std::ofstream File(Directory / L"Scripts/Stomp.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Log = "" } }
function T:OnLanded() self.Properties.Log = self.Properties.Log .. "L;" end
function T:OnStomped(other) self.Properties.Log = self.Properties.Log .. "S:" .. other:GetName() .. ";" end
function T:OnStompedBy(other) self.Properties.Log = self.Properties.Log .. "B:" .. other:GetName() .. ";" end
return T
)";
	}
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -50.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Floor).Size = FVector2(10000.0f, 100.0f);
	const auto Spawn = [&](const char* Name, const FVector3& Position) {
		const FEntity Entity = Scene.CreateEntity(Name);
		Scene.GetTransform(Entity).Position = Position;
		Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Entity).CharacterCollision = FCharacterMovement2DComponent::ECharacterCollision::Block;
		Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Stomp.lua";
		return Entity;
	};
	const FEntity Victim  = Spawn("Victim", FVector3(0.0f, 0.0f, 61.0f));
	const FEntity Stomper = Spawn("Stomper", FVector3(10.0f, 0.0f, 500.0f));
	Scene.UpdateTransforms();

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Directory });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Stomper, "Log").String == "L;S:Victim;");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Victim, "Log").String == "L;B:Stomper;");
	E_EXPECT_NEAR(Scene.GetTransform(Stomper).Position.Z, 179.2f, 2.0f); // 상대 위에 섰다 (가로 10cm 어긋남 → 캡슐 머리 접점 높이)
	E_EXPECT_NEAR(Scene.GetTransform(Stomper).Position.X, 10.0f, 1.0f);  // 둥근 머리에서 미끄러지지 않는다
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

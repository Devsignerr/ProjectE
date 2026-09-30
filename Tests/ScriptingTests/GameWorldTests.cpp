#include "Core/Testing/TestFramework.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>

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
	World.Init({ &Scripts, &Physics, nullptr, nullptr, std::filesystem::temp_directory_path() });

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

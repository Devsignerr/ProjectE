#include "Core/Testing/TestFramework.h"
#include "Network/ReplicationTypes.h"
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

// 클라이언트 역할: 스크립트/게임 모듈은 돌지 않고, 복제 엔티티(NetId)의 동적 바디는 키네마틱으로 트랜스폼을 따른다.
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
	World.Init({ &Scripts, &Physics, nullptr, nullptr, std::filesystem::temp_directory_path() });
	World.BeginPlay(Scene, EWorldRole::Client);
	E_EXPECT_TRUE(World.GetRole() == EWorldRole::Client);
	E_EXPECT_TRUE(Physics.IsActive());
	E_EXPECT_FALSE(Scripts.IsPlaying());
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

// 사망 래그돌 연동 (World/GameWorldRagdoll.cpp): 체력 0 → 켜짐, 리스폰 → 꺼짐 (모든 역할), Lua entity:EnableRagdoll/DisableRagdoll/IsRagdollActive
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/Ragdoll.h"
#include "Scene/Animation.h"
#include "Scene/Components.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <vector>

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	// 다리 둘 + 몸통 + 머리 뼈대 모델 (모델 로더와 같은 모양: 노드 엔티티 계층 + 애니메이션 런타임 + 스킨 Joints)
	FEntity BuildStickFigure(FScene& Scene, FEntity Parent, const char* Name)
	{
		const std::vector<int32>    Parents = { -1, 0, 1, 0, 3, 0, 5 };
		const std::vector<FVector3> Locals  = { { 0, 0, 90 }, { 0, 0, 30 }, { 0, 0, 25 }, { 0, -12, -5 }, { 0, 0, -45 }, { 0, 12, -5 }, { 0, 0, -45 } };
		const FEntity               Root    = Scene.CreateEntity(Name);
		if (Parent.IsValid())
		{
			Scene.SetParent(Root, Parent);
		}
		std::vector<FEntity>   Nodes(Parents.size());
		std::vector<FNodePose> RestPose(Parents.size());
		for (size_t Node = 0; Node < Parents.size(); ++Node)
		{
			Nodes[Node] = Scene.CreateEntity("Bone");
			Scene.SetParent(Nodes[Node], Parents[Node] >= 0 ? Nodes[Parents[Node]] : Root);
			Scene.GetTransform(Nodes[Node]).Position = Locals[Node];
			RestPose[Node].Translation              = Locals[Node];
		}
		FAnimationComponent& Animation  = Scene.GetRegistry().Emplace<FAnimationComponent>(Root);
		Animation.Runtime.Set          = MakeAnimationSet({}, Parents, RestPose);
		Animation.Runtime.NodeEntities = Nodes;
		const FEntity Mesh             = Scene.CreateEntity("Mesh");
		Scene.SetParent(Mesh, Root);
		Scene.GetRegistry().Emplace<FSkinComponent>(Mesh).Joints = Nodes;
		Scene.GetTransform(Root).Rotation = FQuat::FromEuler(0.0f, 0.0f, 20.0f); // 기울여서 쓰러지게
		return Root;
	}
} // namespace

// 체력이 조상(캐릭터 캡슐)에 있는 모델: 죽으면 래그돌, 리스폰(제자리)하면 꺼짐. EnableOnDeath를 끈 모델은 Lua로만
E_TEST(RagdollScript_DeathAndRespawnAndLua)
{
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);

	const FEntity Hero = Scene.CreateEntity("Hero");
	Scene.GetTransform(Hero).Position = FVector3(0.0f, 0.0f, 92.0f);
	Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Hero);
	Scene.GetRegistry().Emplace<FHealthComponent>(Hero).DeathAction = EDeathAction::RespawnInPlace;
	const FEntity HeroModel = BuildStickFigure(Scene, Hero, "HeroModel");
	Scene.GetTransform(HeroModel).Position = FVector3(0.0f, 0.0f, -90.0f);
	Scene.GetRegistry().Emplace<FRagdollComponent>(HeroModel);

	const FEntity Statue = BuildStickFigure(Scene, NullEntity, "Statue");
	Scene.GetTransform(Statue).Position = FVector3(400.0f, 0.0f, 0.0f);
	Scene.GetRegistry().Emplace<FRagdollComponent>(Statue).bEnableOnDeath = false;
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.BeginPlay(Scene);
	World.TickGameplay(Step, nullptr);
	E_EXPECT_FALSE(Physics.IsRagdollActive(Scene, HeroModel));

	Gameplay::ApplyDamage(Scene, Hero, 1000.0f, FEntity());
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Physics.IsRagdollActive(Scene, Hero)); // 조상에서 찾아도 같은 모델
	E_EXPECT_TRUE(Physics.GetRagdollPartCount(Scene, HeroModel) >= 5u);
	const FVector3 HeadStart = Scene.GetTransform(Scene.GetRegistry().Get<FAnimationComponent>(HeroModel).Runtime.NodeEntities[2]).GetWorldPosition();
	for (int32 Frame = 0; Frame < 90; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	const FVector3 HeadNow = Scene.GetTransform(Scene.GetRegistry().Get<FAnimationComponent>(HeroModel).Runtime.NodeEntities[2]).GetWorldPosition();
	E_EXPECT_TRUE(HeadNow.Z < HeadStart.Z - 40.0f);         // 쓰러진다 (자기 캐릭터 캡슐에 걸리지 않는다)
	E_EXPECT_TRUE(Physics.IsRagdollActive(Scene, HeroModel)); // 리스폰 전 (기본 3초)
	for (int32 Frame = 0; Frame < 120; ++Frame)              // 합 3.5초
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_FALSE(Gameplay::IsDead(Scene, Hero));
	E_EXPECT_FALSE(Physics.IsRagdollActive(Scene, HeroModel));
	E_EXPECT_FALSE(Scene.GetRegistry().Get<FAnimationComponent>(HeroModel).Runtime.bPhysicsPose);

	// Lua: 체력 연동을 끈 모델도 직접 켜고 끌 수 있다 (켜진 동안 죽음/리스폰 판단과 무관)
	E_EXPECT_TRUE(Scripts.RunString(R"(
local S = Scene.Find('Statue')
assert(not S:IsRagdollActive())
assert(S:EnableRagdoll() == true)
assert(S:EnableRagdoll() == false)
assert(S:IsRagdollActive())
)"));
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		World.TickGameplay(Step, nullptr);
	}
	E_EXPECT_TRUE(Physics.IsRagdollActive(Scene, Statue));
	E_EXPECT_TRUE(Scripts.RunString("Scene.Find('Statue'):DisableRagdoll(); assert(not Scene.Find('Statue'):IsRagdollActive())"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

// 클라이언트 역할도 복제된 체력으로 같은 판단을 해 로컬 래그돌을 켠다 (래그돌 자세는 복제하지 않는 로컬 연출)
E_TEST(RagdollScript_ClientUsesReplicatedHealth)
{
	FScene        Scene;
	const FEntity Victim = BuildStickFigure(Scene, NullEntity, "Victim");
	Scene.GetRegistry().Emplace<FRagdollComponent>(Victim);
	Scene.GetRegistry().Emplace<FHealthComponent>(Victim);
	Scene.UpdateTransforms();

	FScriptSystem  Scripts;
	FPhysicsSystem Physics;
	FGameWorld     World;
	World.Init({ &Scripts, &Physics, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.BeginPlay(Scene, ENetMode::Client);
	World.TickGameplay(Step, nullptr);
	Scene.GetRegistry().Get<FHealthComponent>(Victim).Health = 0.0f; // 서버에서 복제되어 온 값
	World.TickGameplay(Step, nullptr);
	E_EXPECT_TRUE(Physics.IsRagdollActive(Scene, Victim));
	Scene.GetRegistry().Get<FHealthComponent>(Victim).Health = 100.0f; // 리스폰 복제
	World.TickGameplay(Step, nullptr);
	E_EXPECT_FALSE(Physics.IsRagdollActive(Scene, Victim));
	World.EndPlay();
}

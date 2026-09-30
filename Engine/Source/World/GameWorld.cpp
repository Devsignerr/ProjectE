#include "World/GameWorld.h"

#include "Core/Assert.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimationSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

void FGameWorld::Init(const FGameWorldSystems& InSystems)
{
	E_CHECKF(InSystems.Scripts != nullptr, "FGameWorld: 스크립트 시스템은 필수입니다");
	Systems = InSystems;
	Systems.Scripts->SetContentDirectory(Systems.ContentDirectory);

	FPhysicsSystem* Physics = Systems.Physics;
	if (Physics == nullptr)
	{
		return;
	}
	Systems.Scripts->SetPhysicsHooks({
		[Physics](const FVector3& Origin, const FVector3& Direction, float MaxDistance, FScriptRayHit& OutHit) {
			FPhysicsHit Hit;
			if (!Physics->Raycast(Origin, Direction, MaxDistance, Hit))
			{
				return false;
			}
			OutHit = { Hit.Entity, Hit.Position, Hit.Normal, Hit.Distance };
			return true;
		},
		[Physics](FEntity Entity, const FVector3& Force) { Physics->AddForce(Entity, Force); },
		[Physics](FEntity Entity, const FVector3& Impulse) { Physics->AddImpulse(Entity, Impulse); },
		[Physics](FEntity Entity, const FVector3& Velocity) { Physics->SetVelocity(Entity, Velocity); },
		[Physics](FEntity Entity) { return Physics->GetVelocity(Entity); },
		[Physics](FEntity Entity) { return Physics->GetMass(Entity); },
	});
}

void FGameWorld::BeginPlay(FScene& InScene)
{
	if (IsPlaying())
	{
		EndPlay();
	}
	Scene = &InScene;
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->Begin();
	}
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->BeginPlay(InScene);
	}
	Systems.Scripts->BeginPlay(InScene);
}

void FGameWorld::EndPlay()
{
	if (!IsPlaying())
	{
		return;
	}
	Systems.Scripts->EndPlay();
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->EndPlay(*Scene);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->End();
	}
	Scene = nullptr;
}

void FGameWorld::TickGameplay(float DeltaSeconds, const FInput* Input)
{
	if (!IsPlaying())
	{
		return;
	}
	Systems.Scripts->Update(DeltaSeconds, Input);
	if (Systems.Scripts->ConsumeSceneStructureChanged() && Systems.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	if (Systems.GameModule != nullptr)
	{
		Systems.GameModule->Update(*Scene, DeltaSeconds);
	}
	if (Systems.Physics != nullptr)
	{
		Systems.Physics->Update(*Scene, DeltaSeconds);
	}
	Scene->UpdateTransforms();
}

void FGameWorld::TickPresentation(FScene& TargetScene, float DeltaSeconds)
{
	FAnimationSystem::Update(TargetScene, DeltaSeconds);
	TargetScene.UpdateTransforms();
	if (Systems.Resources != nullptr)
	{
		FSceneAssetResolver::ResolveParticles(TargetScene, *Systems.Resources, Systems.ContentDirectory);
	}
	FParticleSystem::Update(TargetScene, DeltaSeconds);
}

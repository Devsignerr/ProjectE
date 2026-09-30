#include "World/GameWorld.h"

#include "AI/AISystem.h"
#include "Core/Assert.h"
#include "Network/NetDriver.h"
#include "Network/ReplicationTypes.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/AnimationSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

FGameWorld::FGameWorld() : AI(std::make_unique<FAISystem>()) {}
FGameWorld::~FGameWorld() = default;

void FGameWorld::Init(const FGameWorldSystems& InSystems)
{
	E_CHECKF(InSystems.Scripts != nullptr, "FGameWorld: 스크립트 시스템은 필수입니다");
	Systems = InSystems;
	Systems.Scripts->SetContentDirectory(Systems.ContentDirectory);
	AI->SetContentDirectory(Systems.ContentDirectory);

	FPhysicsSystem* Physics = Systems.Physics;
	if (Physics == nullptr)
	{
		AI->SetMovementHooks({});
		return;
	}
	// AI 이동: 동적 강체는 물리에 수평 속도를 넘기고(Z 속도 = 중력/점프는 유지), 그 밖은 AI가 트랜스폼을 옮긴다
	AI->SetMovementHooks({
		[this, Physics](FEntity Entity, const FVector3& DesiredVelocity) {
			const FRigidBodyComponent* Body = Scene != nullptr ? Scene->GetRegistry().TryGet<FRigidBodyComponent>(Entity) : nullptr;
			if (Body == nullptr || Body->MotionType != static_cast<int32>(EPhysicsMotionType::Dynamic))
			{
				return false;
			}
			const FVector3 Current = Physics->GetVelocity(Entity);
			Physics->SetVelocity(Entity, FVector3(DesiredVelocity.X, DesiredVelocity.Y, Current.Z));
			return true;
		},
	});
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

void FGameWorld::BeginPlay(FScene& InScene, ENetMode InMode)
{
	if (IsPlaying())
	{
		EndPlay();
	}
	Scene                 = &InScene;
	Mode                  = InMode;
	const bool bClient    = Mode == ENetMode::Client;
	const bool bDedicated = Mode == ENetMode::DedicatedServer;

	// 스크립트 네트워크 정보: 실행 위치 필터, 모드, 로컬 플레이어, 소유권(가장 가까운 복제 조상의 OwnerPlayerId)
	FScriptNetHooks NetHooks;
	NetHooks.bRunServerScripts = !bClient;
	NetHooks.bRunClientScripts = !bDedicated;
	NetHooks.bIsServer         = !bClient;
	NetHooks.bIsClient         = !bDedicated;
	NetHooks.ModeName          = ToString(Mode);
	NetHooks.GetLocalPlayerId  = [this]() {
		if (Mode == ENetMode::DedicatedServer)
		{
			return -1; // 전용 서버에는 로컬 플레이어가 없다
		}
		return static_cast<int32>(Systems.Net != nullptr ? Systems.Net->GetLocalPlayerId() : 0);
	};
	NetHooks.GetOwner          = [this](FEntity Entity) {
		for (FEntity Current = Entity; Scene != nullptr && Scene->GetRegistry().IsValid(Current); Current = Scene->GetParent(Current))
		{
			if (const FReplicatedComponent* Replicated = Scene->GetRegistry().TryGet<FReplicatedComponent>(Current))
			{
				return Replicated->OwnerPlayerId;
			}
		}
		return -1;
	};
	Systems.Scripts->SetNetHooks(std::move(NetHooks));

	if (Systems.Physics != nullptr)
	{
		if (bClient)
		{
			// 서버가 시뮬레이션하는 복제 엔티티(NetId 보유)는 키네마틱: 복제 트랜스폼을 따라가며 로컬 물체와 충돌
			Systems.Physics->SetKinematicOverride([](const FScene& Target, FEntity Entity) { return Target.GetRegistry().Has<FNetIdComponent>(Entity); });
		}
		else
		{
			Systems.Physics->SetKinematicOverride(nullptr);
		}
		Systems.Physics->Begin();
	}
	if (Systems.GameModule != nullptr && !bClient) // 게임 모듈(C++ 게임 로직)은 서버에서만
	{
		Systems.GameModule->BeginPlay(InScene);
	}
	if (!bClient) // AI도 서버에서만. 스크립트 OnStart가 블랙보드를 쓸 수 있게 스크립트보다 먼저
	{
		AI->Begin(InScene);
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
	AI->End();
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
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
	Systems.Scripts->Update(DeltaSeconds, Input); // 실행 위치 필터는 BeginPlay에서 정했다
	if (Systems.Scripts->ConsumeSceneStructureChanged() && Systems.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(*Scene, *Systems.Resources, Systems.ContentDirectory);
	}
	if (Systems.GameModule != nullptr && Mode != ENetMode::Client)
	{
		Systems.GameModule->Update(*Scene, DeltaSeconds);
	}
	AI->Update(*Scene, DeltaSeconds); // Client 역할은 Begin하지 않았으므로 아무것도 하지 않는다
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

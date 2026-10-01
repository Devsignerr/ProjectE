// FGameWorld의 사망 래그돌 연동 (규칙은 Physics/Ragdoll.h).
//   FRagdollComponent(bEnableOnDeath) 모델마다 자신이나 가장 가까운 조상의 FHealthComponent를 보고, 살아 있다 → 죽음이면 켜고
//   죽음 → 살아 있음(리스폰)이면 끈다. 상태 변화로만 움직이므로 스크립트가 entity:EnableRagdoll()로 직접 켠 것은 그대로 둔다.
//   모든 역할에서 돈다: 클라이언트는 복제된 Health로 같은 판단을 하고 각자 로컬로 시뮬레이션한다 (래그돌 자세는 복제하지 않는다 —
//   쓰러지는 모양은 클라이언트마다 조금 다를 수 있다). 게임플레이 규칙(사망·리스폰 처리) 뒤, 물리 앞
#include "World/GameWorld.h"

#include "Physics/PhysicsSystem.h"
#include "Physics/Ragdoll.h"
#include "Scene/Gameplay.h"
#include "Scene/Scene.h"

#include <vector>

void FGameWorld::TickRagdolls()
{
	FPhysicsSystem* Physics = Systems.Physics;
	if (Physics == nullptr || !Physics->IsActive())
	{
		return;
	}
	FRegistry&           Registry = Scene->GetRegistry();
	std::vector<FEntity> Models;
	Registry.View<FRagdollComponent>().Each([&](FEntity Entity, FRagdollComponent& Ragdoll) {
		if (Ragdoll.bEnableOnDeath)
		{
			Models.push_back(Entity);
		}
	});
	for (const FEntity Model : Models)
	{
		const FHealthComponent* Health = nullptr;
		for (FEntity Current = Model; Current.IsValid() && Registry.IsValid(Current) && Health == nullptr; Current = Scene->GetParent(Current))
		{
			Health = Registry.TryGet<FHealthComponent>(Current);
		}
		const bool bDead     = Health != nullptr && Gameplay::IsDead(*Health);
		const auto Previous  = RagdollDeadStates.find(Model);
		const bool bWasDead  = Previous != RagdollDeadStates.end() && Previous->second;
		RagdollDeadStates[Model] = bDead;
		if (bDead && !bWasDead)
		{
			Physics->EnableRagdoll(*Scene, Model);
		}
		else if (!bDead && bWasDead)
		{
			Physics->DisableRagdoll(*Scene, Model);
		}
	}
	// 사라진 모델
	for (auto It = RagdollDeadStates.begin(); It != RagdollDeadStates.end();)
	{
		It = Registry.IsValid(It->first) ? std::next(It) : RagdollDeadStates.erase(It);
	}
}

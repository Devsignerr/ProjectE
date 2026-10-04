// FGameWorld의 물리 알림 전달 (충돌/트리거 — 규칙은 Physics/PhysicsSystem.h "충돌 알림", 이벤트 형식은 Scene/CollisionEvents.h).
//
// 시점: 게임플레이 틱의 물리 → UpdateTransforms 뒤, 스크립트 OnLateUpdate 앞. 물리 스텝 중(Jolt 작업 스레드)에는 아무것도 부르지 않는다.
// 받는 쪽: 이벤트의 Self 엔티티 스크립트 (Lua OnCollisionBegin(other, info)/OnCollisionEnd(other)/OnTriggerEnter(other)/OnTriggerExit(other),
//   관절이 끊어지면 관절 엔티티의 OnJointBreak(other, force) — force는 N)
//   → 게임 모듈 OnCollisionBegin 등. 쌍 하나는 양쪽 엔티티가 한 번씩 받는다. 처리 중 파괴된 엔티티는 건너뛰고, 상대가 없으면 other = nil.
// 보고 대상(비용): 트리거·ReportContacts 강체 + 여기서 정하는 필터 = 스크립트가 붙은 엔티티, 게임 모듈 WantsCollisionEvents.
// 역할: 서버/Standalone은 전부. 클라이언트는 게임 로직(서버 권위)을 내지 않는다 — 복제 엔티티(자신이나 조상에 NetId)의 이벤트는
//   만들지도 전달하지도 않고(복제 바디는 키네마틱으로 서버 결과를 따라갈 뿐), 클라이언트에만 있는 로컬 엔티티(ClientOnly 연출 등)의
//   스크립트만 받는다. 게임 모듈은 클라이언트에서 돌지 않는다
#include "World/GameWorld.h"

#include "Network/ReplicationTypes.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <vector>

namespace
{
	// 자신이나 조상이 복제 엔티티인가
	bool IsReplicatedEntity(const FScene& Scene, FEntity Entity)
	{
		const FRegistry& Registry = Scene.GetRegistry();
		for (FEntity Current = Entity; Current.IsValid() && Registry.IsValid(Current); Current = Scene.GetParent(Current))
		{
			if (Registry.Has<FNetIdComponent>(Current))
			{
				return true;
			}
		}
		return false;
	}

	FGameRpcValue EntityOrNil(const FScene& Scene, FEntity Entity)
	{
		return Entity.IsValid() && Scene.GetRegistry().IsValid(Entity) ? FGameRpcValue::MakeEntity(Entity) : FGameRpcValue();
	}
} // namespace

bool FGameWorld::ShouldReportContacts(const FScene& Target, FEntity Entity) const
{
	if (Mode == ENetMode::Client)
	{
		return Target.GetRegistry().Has<FScriptComponent>(Entity) && !IsReplicatedEntity(Target, Entity);
	}
	return Target.GetRegistry().Has<FScriptComponent>(Entity) || (Systems.GameModule != nullptr && Systems.GameModule->WantsCollisionEvents(Target, Entity));
}

bool FGameWorld::DispatchCollisionEvents()
{
	if (Systems.Physics == nullptr || Systems.Physics->GetCollisionEvents().empty())
	{
		return false;
	}
	const std::vector<FCollisionEvent> Events = Systems.Physics->GetCollisionEvents(); // 처리 중 다음 물리 갱신이 없으므로 복사 한 번이면 된다
	const FRegistry&                   Registry = Scene->GetRegistry();
	const bool                         bClient  = Mode == ENetMode::Client;
	for (const FCollisionEvent& Event : Events)
	{
		if (!Event.Self.IsValid() || !Registry.IsValid(Event.Self))
		{
			continue; // 받는 쪽이 이미 없음 (앞 이벤트 처리 중 파괴 포함)
		}
		if (bClient && IsReplicatedEntity(*Scene, Event.Self))
		{
			continue;
		}
		const FGameRpcValue Other = EntityOrNil(*Scene, Event.Other);
		switch (Event.Type)
		{
		case ECollisionEventType::CollisionBegin:
			Systems.Scripts->InvokeMethodWithFields(Event.Self, "OnCollisionBegin", { Other },
			                                        { { "Point", FGameRpcValue::MakeVector3(Event.Point) },
			                                          { "Normal", FGameRpcValue::MakeVector3(Event.Normal) },
			                                          { "Impulse", FGameRpcValue::MakeNumber(Event.Impulse) },
			                                          { "Speed", FGameRpcValue::MakeNumber(Event.ApproachSpeed) } });
			break;
		case ECollisionEventType::CollisionEnd: Systems.Scripts->InvokeMethod(Event.Self, "OnCollisionEnd", { Other }); break;
		case ECollisionEventType::TriggerEnter: Systems.Scripts->InvokeMethod(Event.Self, "OnTriggerEnter", { Other }); break;
		case ECollisionEventType::TriggerExit:  Systems.Scripts->InvokeMethod(Event.Self, "OnTriggerExit", { Other }); break;
		case ECollisionEventType::JointBreak:
			Systems.Scripts->InvokeMethod(Event.Self, "OnJointBreak", { Other, FGameRpcValue::MakeNumber(Event.Impulse) });
			break;
		default:                                break;
		}
		if (!bClient && Systems.GameModule != nullptr && Registry.IsValid(Event.Self))
		{
			Systems.GameModule->CollisionEvent(*Scene, Event);
		}
	}
	return true;
}

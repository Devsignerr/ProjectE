// Lua 애니메이션 그래프 바인딩 (Scene/AnimGraph.h). 대상 = AnimGraphComponent가 있는 엔티티 또는 그 조상 (캐릭터 루트에서 불러도 된다)
//   entity:SetAnimParam(name, number|bool) → 그래프를 찾았는지   entity:GetAnimParam(name) → number / bool(그래프에 Bool로 선언) / nil
//   entity:GetAnimState() → 현재 상태 이름 (그래프가 없거나 아직 시작 전이면 nil)
//   파라미터는 이 프로세스에만 있다 (복제되지 않음) — 멀티플레이는 Both 스크립트에서 쓰거나 복제 값으로 계산한다
// 몽타주 (Scene/AnimMontage.h, 로컬 전용). 대상 = AnimationComponent가 있는 엔티티 또는 그 조상
//   entity:PlayMontage(clip, {Slot=, BlendIn=, BlendOut=, Speed=, StartTime=, EndTime=, Loop=}) → 재생했는지
//   entity:StopMontage([slot], [blendOut]) → 멈춘 것이 있는지   entity:IsMontagePlaying([slot]) → bool
//   끝나면 function T:OnMontageEnded(clip, interrupted, slot) end (모델 루트 또는 가장 가까운 조상의 스크립트, 다음 프레임)
// 시선 IK (Scene/AnimIK.h): entity:SetLookAtTarget(Vector3 | entity | nil) / ClearLookAtTarget() → LookAtComponent를 찾았는지
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>

void FLuaRuntime::RegisterAnimationGraphBindings()
{
	const auto Require = [this](const FScriptEntity& Entity) -> FScene& {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity.Entity))
		{
			throw std::runtime_error("유효하지 않은 엔티티입니다");
		}
		return *Scene;
	};

	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["SetAnimParam"] = [Require](const FScriptEntity& Entity, const std::string& Name, sol::object Value) {
		FScene& Target = Require(Entity);
		if (Value.is<bool>())
		{
			return FAnimationSystem::SetAnimParam(Target, Entity.Entity, Name, Value.as<bool>());
		}
		if (Value.get_type() == sol::type::number)
		{
			return FAnimationSystem::SetAnimParam(Target, Entity.Entity, Name, Value.as<float>());
		}
		throw std::runtime_error("SetAnimParam: 값은 숫자나 bool이어야 합니다");
	};
	EntityType["GetAnimParam"] = [this, Require](const FScriptEntity& Entity, const std::string& Name) -> sol::object {
		FScene&                    Target = Require(Entity);
		const std::optional<float> Value  = FAnimationSystem::GetAnimParam(Target, Entity.Entity, Name);
		if (!Value)
		{
			return sol::lua_nil;
		}
		if (FAnimationSystem::IsAnimParamBool(Target, Entity.Entity, Name))
		{
			return sol::make_object(Lua, *Value != 0.0f);
		}
		return sol::make_object(Lua, *Value);
	};
	EntityType["GetAnimState"] = [this, Require](const FScriptEntity& Entity) -> sol::object {
		const std::string State = FAnimationSystem::GetAnimState(Require(Entity), Entity.Entity);
		return State.empty() ? sol::object(sol::lua_nil) : sol::make_object(Lua, State);
	};

	// 몽타주 (Scene/AnimMontage.h)
	EntityType["PlayMontage"] = [Require](const FScriptEntity& Entity, const std::string& Clip, sol::optional<sol::table> Options) {
		FMontagePlayParams Params;
		if (Options)
		{
			const sol::table& Table = *Options;
			Params.Slot      = Table.get_or<std::string>("Slot", Params.Slot);
			Params.BlendIn   = Table.get_or("BlendIn", Params.BlendIn);
			Params.BlendOut  = Table.get_or("BlendOut", Params.BlendOut);
			Params.Speed     = Table.get_or("Speed", Params.Speed);
			Params.StartTime = Table.get_or("StartTime", Params.StartTime);
			Params.EndTime   = Table.get_or("EndTime", Params.EndTime);
			Params.bLoop     = Table.get_or("Loop", Params.bLoop);
		}
		return FAnimationSystem::PlayMontage(Require(Entity), Entity.Entity, Clip, Params);
	};
	EntityType["StopMontage"] = [Require](const FScriptEntity& Entity, sol::optional<std::string> Slot, sol::optional<float> BlendOut) {
		return FAnimationSystem::StopMontage(Require(Entity), Entity.Entity, Slot.value_or(std::string()), BlendOut.value_or(-1.0f));
	};
	EntityType["IsMontagePlaying"] = [Require](const FScriptEntity& Entity, sol::optional<std::string> Slot) {
		return FAnimationSystem::IsMontagePlaying(Require(Entity), Entity.Entity, Slot.value_or(std::string()));
	};

	// 시선 IK (Scene/AnimIK.h)
	EntityType["SetLookAtTarget"] = [Require](const FScriptEntity& Entity, sol::object Target) {
		FScene& Owner = Require(Entity);
		if (Target.is<FVector3>())
		{
			return FAnimationSystem::SetLookAtTarget(Owner, Entity.Entity, Target.as<FVector3>());
		}
		if (Target.is<FScriptEntity>())
		{
			return FAnimationSystem::SetLookAtTargetEntity(Owner, Entity.Entity, Target.as<FScriptEntity>().Entity);
		}
		if (!Target.valid() || Target.get_type() == sol::type::lua_nil)
		{
			return FAnimationSystem::ClearLookAtTarget(Owner, Entity.Entity);
		}
		throw std::runtime_error("SetLookAtTarget: Vector3, 엔티티 또는 nil이어야 합니다");
	};
	EntityType["ClearLookAtTarget"] = [Require](const FScriptEntity& Entity) {
		return FAnimationSystem::ClearLookAtTarget(Require(Entity), Entity.Entity);
	};
}

void FLuaRuntime::DispatchMontageEvents()
{
	FRegistry&                     Registry = Scene->GetRegistry();
	std::vector<FAnimMontageEvent> Events;
	Registry.View<FAnimationComponent>().Each([&](FEntity, FAnimationComponent& Animation) {
		Events.insert(Events.end(), Animation.Runtime.PendingMontageEvents.begin(), Animation.Runtime.PendingMontageEvents.end());
	});
	for (const FAnimMontageEvent& Event : Events)
	{
		// 받는 쪽: 모델 루트의 스크립트, 없으면 가장 가까운 조상의 스크립트 (노티파이와 같음)
		for (FEntity Current = Event.Entity; Current.IsValid() && Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			const auto Found = Instances.find(Current.ToId());
			if (Found == Instances.end())
			{
				continue;
			}
			FScriptInstance& Instance = Found->second;
			if (Instance.bFaulted || !Instance.bStarted)
			{
				break;
			}
			const sol::object Method = Instance.Self["OnMontageEnded"];
			if (Method.get_type() == sol::type::function)
			{
				const sol::table               Self = Instance.Self;
				sol::protected_function        Function(Method.as<sol::function>(), Traceback);
				sol::protected_function_result Result = Function(Self, Event.Clip, Event.bInterrupted, Event.Slot);
				if (!Result.valid())
				{
					const sol::error Error = Result;
					Instance.bFaulted      = true;
					ReportError(std::format("스크립트 오류 ({}:OnMontageEnded) — 이 인스턴스는 멈춥니다 (스크립트 저장 시 재개)\n{}", Instance.ScriptAsset, Error.what()));
				}
			}
			break;
		}
	}
}

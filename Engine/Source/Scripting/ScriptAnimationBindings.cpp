// Lua 애니메이션 그래프 바인딩 (Scene/AnimGraph.h). 대상 = AnimGraphComponent가 있는 엔티티 또는 그 조상 (캐릭터 루트에서 불러도 된다)
//   entity:SetAnimParam(name, number|bool) → 그래프를 찾았는지   entity:GetAnimParam(name) → number / bool(그래프에 Bool로 선언) / nil
//   entity:GetAnimState() → 현재 상태 이름 (그래프가 없거나 아직 시작 전이면 nil)
//   파라미터는 이 프로세스에만 있다 (복제되지 않음) — 멀티플레이는 Both 스크립트에서 쓰거나 복제 값으로 계산한다
#include "Scene/AnimationSystem.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

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
}

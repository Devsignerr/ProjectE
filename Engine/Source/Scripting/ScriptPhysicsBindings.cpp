// Lua 물리 알림 전달 (FGameWorld가 물리 스텝 뒤에 부른다 — 규칙은 Physics/PhysicsSystem.h "충돌 알림")
//   function T:OnCollisionBegin(other, info) end  -- info.Point (Vector3, cm), info.Normal (나를 상대에서 밀어내는 방향),
//                                                 --   info.Impulse (충격 세기 추정 kg·cm/s), info.Speed (다가오던 속력 cm/s)
//   function T:OnCollisionEnd(other) end          -- other는 파괴됐으면 nil
//   function T:OnTriggerEnter(other) end / function T:OnTriggerExit(other) end  -- 트리거 쪽과 들어온 쪽 둘 다 받는다
// 래그돌 (Physics/Ragdoll.h — 엔티티 자신이나 자손의 스켈레탈 모델, 물리 훅이 없으면 false/무시):
//   entity:EnableRagdoll() → 켰는가 (이미 켜져 있거나 뼈대가 없으면 false)   entity:DisableRagdoll()   entity:IsRagdollActive()
//   각 프로세스 로컬 연출이다 (복제되지 않음). 사망 연동은 RagdollComponent.EnableOnDeath가 자동으로 한다
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>

void FLuaRuntime::RegisterPhysicsBindings()
{
	const auto Require = [this](const FScriptEntity& Entity) {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity.Entity))
		{
			throw std::runtime_error("유효하지 않은 엔티티입니다");
		}
	};
	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["EnableRagdoll"] = [this, Require](const FScriptEntity& Entity) {
		Require(Entity);
		return PhysicsHooks != nullptr && PhysicsHooks->EnableRagdoll && PhysicsHooks->EnableRagdoll(Entity.Entity);
	};
	EntityType["DisableRagdoll"] = [this, Require](const FScriptEntity& Entity) {
		Require(Entity);
		if (PhysicsHooks != nullptr && PhysicsHooks->DisableRagdoll)
		{
			PhysicsHooks->DisableRagdoll(Entity.Entity);
		}
	};
	EntityType["IsRagdollActive"] = [this, Require](const FScriptEntity& Entity) {
		Require(Entity);
		return PhysicsHooks != nullptr && PhysicsHooks->IsRagdollActive && PhysicsHooks->IsRagdollActive(Entity.Entity);
	};
}

bool FLuaRuntime::InvokeMethodWithFields(FEntity Target, const std::string& MethodName, const FGameRpcArgs& Args, const FScriptEventFields& Fields)
{
	const auto Found = Instances.find(Target.ToId());
	if (Found == Instances.end() || Found->second.bFaulted || !Found->second.Self.valid())
	{
		return false;
	}
	FScriptInstance&  Instance = Found->second;
	const sol::object Method   = Instance.Self[MethodName];
	if (Method.get_type() != sol::type::function)
	{
		return false;
	}
	std::vector<sol::object> Values;
	Values.reserve(Args.size() + 1);
	for (const FGameRpcValue& Arg : Args)
	{
		Values.push_back(FromRpcValue(Arg));
	}
	sol::table Table = Lua.create_table();
	for (const auto& [Key, Value] : Fields)
	{
		Table[Key] = FromRpcValue(Value);
	}
	Values.push_back(Table);
	const sol::table               Self = Instance.Self;
	sol::protected_function        Function(Method.as<sol::function>(), Traceback);
	sol::protected_function_result Result = Function(Self, sol::as_args(Values));
	if (!Result.valid())
	{
		const sol::error Error = Result;
		Instance.bFaulted      = true;
		ReportError(std::format("스크립트 오류 ({}:{}) — 이 인스턴스는 멈춥니다 (스크립트 저장 시 재개)\n{}", Instance.ScriptAsset, MethodName, Error.what()));
		return false;
	}
	return true;
}

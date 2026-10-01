// Lua 물리 알림 전달 (FGameWorld가 물리 스텝 뒤에 부른다 — 규칙은 Physics/PhysicsSystem.h "충돌 알림")
//   function T:OnCollisionBegin(other, info) end  -- info.Point (Vector3, cm), info.Normal (나를 상대에서 밀어내는 방향),
//                                                 --   info.Impulse (충격 세기 추정 kg·cm/s), info.Speed (다가오던 속력 cm/s)
//   function T:OnCollisionEnd(other) end          -- other는 파괴됐으면 nil
//   function T:OnTriggerEnter(other) end / function T:OnTriggerExit(other) end  -- 트리거 쪽과 들어온 쪽 둘 다 받는다
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>

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

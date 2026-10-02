// Lua AI 바인딩 (블랙보드, 이동, 경로)과 스크립트 객체(Lua 비헤이비어 트리 노드). AI 기능은 FScriptAIHooks로만 접근한다.
#include "Core/Log.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <format>
#include <stdexcept>

E_DECLARE_LOG_CATEGORY(LogScript)

void FLuaRuntime::RegisterAIBindings()
{
	const auto RequireEntity = [this](FEntity Entity) {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity))
		{
			throw std::runtime_error("파괴되었거나 유효하지 않은 엔티티입니다");
		}
	};

	// ---- 블랙보드: entity:GetBlackboard():Get("Key") / :Set("Key", 값) / :IsSet / :Clear
	//   값: bool/number/string/Vector3/Entity. Set(key, nil)은 Clear. 트리/키가 없거나 타입이 다르면 Set은 false, Get은 nil
	Lua.new_usertype<FScriptBlackboard>(
		"Blackboard",
		sol::no_constructor,
		"Get", [this](const FScriptBlackboard& Blackboard, const std::string& Key) -> sol::object {
			FScriptValue Value;
			FEntity      Entity;
			bool         bIsEntity = false;
			if (!AIHooks || !AIHooks->GetBlackboard || !AIHooks->GetBlackboard(Blackboard.Entity, Key, Value, Entity, bIsEntity))
			{
				return sol::lua_nil;
			}
			if (bIsEntity)
			{
				return Scene != nullptr && Scene->GetRegistry().IsValid(Entity) ? sol::make_object(Lua, FScriptEntity{ Entity }) : sol::object(sol::lua_nil);
			}
			return FromScriptValue(Value);
		},
		"Set", [this](const FScriptBlackboard& Blackboard, const std::string& Key, sol::object Value) {
			if (!AIHooks)
			{
				return false;
			}
			if (Value.get_type() == sol::type::lua_nil)
			{
				return AIHooks->ClearBlackboard && AIHooks->ClearBlackboard(Blackboard.Entity, Key);
			}
			if (Value.is<FScriptEntity>())
			{
				return AIHooks->SetBlackboardEntity && AIHooks->SetBlackboardEntity(Blackboard.Entity, Key, Value.as<FScriptEntity>().Entity);
			}
			const FScriptValue Converted = ToScriptValue(Value);
			if (Converted.IsNil())
			{
				throw std::runtime_error(std::format("블랙보드 '{}'에 쓸 수 없는 값입니다 (bool/number/string/Vector3/Entity)", Key));
			}
			return AIHooks->SetBlackboard && AIHooks->SetBlackboard(Blackboard.Entity, Key, Converted);
		},
		"IsSet", [this](const FScriptBlackboard& Blackboard, const std::string& Key) {
			FScriptValue Value;
			FEntity      Entity;
			bool         bIsEntity = false;
			return AIHooks && AIHooks->GetBlackboard && AIHooks->GetBlackboard(Blackboard.Entity, Key, Value, Entity, bIsEntity);
		},
		"Clear", [this](const FScriptBlackboard& Blackboard, const std::string& Key) {
			return AIHooks && AIHooks->ClearBlackboard && AIHooks->ClearBlackboard(Blackboard.Entity, Key);
		});

	const auto MoveTo = [this, RequireEntity](FEntity Entity, const FVector3& Goal, sol::optional<float> AcceptanceRadius) {
		RequireEntity(Entity);
		return AIHooks && AIHooks->MoveTo ? AIHooks->MoveTo(Entity, Goal, AcceptanceRadius.value_or(-1.0f)) : std::string("Failed");
	};

	// ---- 엔티티: 트리 제어와 이동 (서버/Standalone에서만 의미가 있다 — 클라이언트는 AI가 돌지 않아 "Failed"/"Idle")
	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["GetBlackboard"] = [RequireEntity](const FScriptEntity& Entity) {
		RequireEntity(Entity.Entity);
		return FScriptBlackboard{ Entity.Entity };
	};
	EntityType["StartBehaviorTree"] = [this, RequireEntity](const FScriptEntity& Entity) {
		RequireEntity(Entity.Entity);
		return AIHooks && AIHooks->StartTree && AIHooks->StartTree(Entity.Entity);
	};
	EntityType["StopBehaviorTree"] = [this, RequireEntity](const FScriptEntity& Entity) {
		RequireEntity(Entity.Entity);
		if (AIHooks && AIHooks->StopTree)
		{
			AIHooks->StopTree(Entity.Entity);
		}
	};
	// entity:MoveTo(Vector3, 도착 반경?) → "Moving"/"Succeeded"/"Failed". 진행은 entity:GetMoveStatus()
	EntityType["MoveTo"] = [MoveTo](const FScriptEntity& Entity, const FVector3& Goal, sol::optional<float> AcceptanceRadius) {
		return MoveTo(Entity.Entity, Goal, AcceptanceRadius);
	};
	EntityType["GetMoveStatus"] = [this, RequireEntity](const FScriptEntity& Entity) {
		RequireEntity(Entity.Entity);
		return AIHooks && AIHooks->GetMoveStatus ? AIHooks->GetMoveStatus(Entity.Entity) : std::string("Idle");
	};
	EntityType["StopMove"] = [this, RequireEntity](const FScriptEntity& Entity) {
		RequireEntity(Entity.Entity);
		if (AIHooks && AIHooks->StopMove)
		{
			AIHooks->StopMove(Entity.Entity);
		}
	};

	// ---- AI.FindPath(시작, 끝) → Vector3 배열(시작·끝 포함) 또는 nil, AI.MoveTo(entity, 목표, 반경?) = entity:MoveTo
	sol::table AITable  = Lua.create_named_table("AI");
	AITable["FindPath"] = [this](const FVector3& Start, const FVector3& End) -> sol::object {
		std::vector<FVector3> Points;
		if (!AIHooks || !AIHooks->FindPath || !AIHooks->FindPath(Start, End, Points))
		{
			return sol::lua_nil;
		}
		sol::table Result = Lua.create_table(static_cast<int>(Points.size()), 0);
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			Result[Index + 1] = Points[Index];
		}
		return Result;
	};
	AITable["MoveTo"] = [MoveTo](const FScriptEntity& Entity, const FVector3& Goal, sol::optional<float> AcceptanceRadius) {
		return MoveTo(Entity.Entity, Goal, AcceptanceRadius);
	};
}

// ---------------------------------------------------------------- 스크립트 객체

uint32 FLuaRuntime::CreateObject(const std::string& ScriptAsset, const std::string& Overrides, FEntity Entity)
{
	FScriptClass& Class = LoadClass(ScriptAsset);
	if (!Class.bValid)
	{
		return 0; // 로드 오류는 LoadClass가 한 번 보고한다
	}
	sol::table Self          = Lua.create_table();
	Self["entity"]           = FScriptEntity{ Entity };
	Self["Properties"]       = MakeProperties(Class, ScriptAsset, Overrides);
	Self[sol::metatable_key] = Class.Metatable;

	uint32 Id = NextObjectId++;
	if (Id == 0)
	{
		Id = NextObjectId++; // 0은 무효 핸들
	}
	Objects[Id] = FScriptObject{ ScriptAsset, Self, false };
	return Id;
}

bool FLuaRuntime::CallObject(uint32 Id, const char* Method, const float* DeltaSeconds, FScriptValue& OutResult, bool* bOutFound)
{
	OutResult = FScriptValue{};
	if (bOutFound)
	{
		*bOutFound = false;
	}
	const auto Found = Objects.find(Id);
	if (Found == Objects.end() || Found->second.bFaulted)
	{
		return false;
	}
	const sol::object Function = Found->second.Self[Method];
	if (Function.get_type() != sol::type::function)
	{
		return false;
	}
	if (bOutFound)
	{
		*bOutFound = true;
	}

	// 호출 중 Objects가 바뀔 수 있으므로(다른 노드 생성 등) 필요한 값은 복사해 둔다
	const sol::table               Self = Found->second.Self;
	const std::string              ScriptAsset = Found->second.ScriptAsset;
	sol::protected_function        Protected(Function.as<sol::function>(), Traceback);
	const FInstanceScope           Scope(*this, NullEntity); // 스크립트 객체는 인스턴스가 아니다 (Timer/Coroutine 사용 불가)
	sol::protected_function_result Result = DeltaSeconds ? Protected(Self, *DeltaSeconds) : Protected(Self);
	if (!Result.valid())
	{
		const sol::error Error = Result;
		if (const auto Again = Objects.find(Id); Again != Objects.end())
		{
			Again->second.bFaulted = true;
		}
		ReportError(std::format("스크립트 오류 ({}:{}) — 이 노드는 멈춥니다 (스크립트 저장 시 재개)\n{}", ScriptAsset, Method, Error.what()));
		return false;
	}
	if (Result.return_count() > 0)
	{
		OutResult = ToScriptValue(Result.get<sol::object>());
	}
	return true;
}

void FLuaRuntime::DestroyObject(uint32 Id)
{
	Objects.erase(Id);
}

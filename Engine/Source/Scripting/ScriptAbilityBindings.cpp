// Lua 능력 시스템 바인딩 (Scene/Ability, Phase 53 사이드) + 능력 스크립트 호스트.
//
// 엔티티 (능력 시스템 컴포넌트가 있는 엔티티. 없으면 nil/false/0)
//   entity:HasAbilitySystem()                         entity:TryActivateAbility(이름) → 성공, 이유  (소유 클라이언트는 예측 발동 + 서버 요청)
//   entity:CancelAbility(이름) / IsAbilityActive(이름) / GetAbilityCooldown(이름) → 남은 초, 전체 초   entity:GetAbilities() → 이름 배열
//   entity:GetAttribute(이름) / GetBaseAttribute(이름) → 수 또는 nil       서버만: entity:SetBaseAttribute(이름, 값), GiveAbility/RemoveAbility(이름)
//   entity:HasTag(태그) (부모 질의 포함) / HasTagExact(태그) / GetTagCount(태그)   서버만: entity:AddLooseTag(태그[, 수]) / RemoveLooseTag
//   서버만: entity:ApplyEffect(효과[, { Source = 엔티티, Level = 수, SetByCaller = { 이름 = 값 } }]) → 적용됨, 핸들(지속 효과)
//           entity:RemoveEffect(핸들), entity:RemoveEffectsWithTags(태그 또는 배열) → 지운 수
//   entity:GetActiveEffects() → { { Name, Handle, Stacks, Remaining(-1 = 무한), Duration, Predicted }, ... }
//   이벤트(엔티티 스크립트 메서드, FGameWorld가 전달): OnAttributeChanged(이름, 새 값, 이전 값), OnTagChanged(태그, 수),
//     OnAbilityActivated(이름), OnAbilityEnded(이름, 취소됨), OnAbilityFailed(이름, 이유)
// Abilities 테이블: Abilities.FindInRadius(중심, 반경[, 제외 엔티티]) → 능력 시스템 엔티티 배열(가까운 순, 물리 없이 위치로),
//   Abilities.FindLocal() → 이 프로세스가 조종하는 입력 폰(AcceptInput), Abilities.Describe(엔티티) → 디버그 글자
//
// 능력 스크립트 (능력 표 Script 칸): 클래스 테이블을 반환한다. 발동마다 새 self(Properties = 선언 기본값, self.entity = 소유자)
//   function MyAbility:OnActivate(ctx) ... end   -- 코루틴으로 실행. 반환하면 능력이 끝난다 (ctx:EndAbility()로 일찍 끝낼 수 있다)
//   function MyAbility:OnEnd(ctx, cancelled) end -- 끝/취소 뒤 (코루틴 밖)
//   ctx 필드: Owner(엔티티), Ability(이름), Level, Id / 메서드:
//     ctx:Wait(초) / WaitFrames(n) / WaitUntil(함수) / WaitAnimNotify(이름[, 제한 초]) → 노티파이가 왔으면 true (소유자와 자손 모델)
//     ctx:PlayMontage(...) = Owner:PlayMontage(...) (로컬 연출 — 서버와 예측 클라이언트가 각자)
//     ctx:ApplyEffectToSelf(효과[, opts]) / ctx:ApplyEffectToTarget(대상, 효과[, opts]) → 적용됨, 핸들
//     ctx:CommitAbility() (= CommitCost/CommitCooldown — 비용·쿨다운·ActivationEffects를 함께, ManualCommit 능력용)
//     ctx:EndAbility(), ctx:IsActive(), ctx:HasAuthority(), ctx:IsPredicting(), ctx:IsLocallyControlled(), ctx:GetAttribute(이름)
// 규칙
//   - 서버(권한)는 항상, 소유 클라이언트는 예측 능력이면 같은 스크립트를 로컬로 돌린다. 다른 대상 효과·스폰·데미지는
//     if ctx:HasAuthority() then ... end 안에 둔다 (클라이언트에서 ApplyEffectToTarget은 아무것도 하지 않고 false).
//   - 예측 창 = OnActivate의 첫 대기 전. 그 안의 자기 효과만 예측하고 서버 사본에 같은 예측 키가 붙는다(규칙은 AbilitySystem.cpp).
//   - 시간 = 능력 틱 시간 (FGameWorld 게임플레이 틱 dt, MaxDeltaSeconds로 제한). 대기 재개는 능력 틱(스크립트 갱신 뒤, 캐릭터 이동 전).
//   - 실행 중 FInstanceScope = 소유 엔티티 (소유자에게 스크립트 인스턴스가 있으면 Timer/Coroutine/SpawnPrefab 콜백은 그 인스턴스 소유).
//   - 오류는 그 능력만 취소한다 (스크립트 오류 수에 더함). 플레이가 끝나면 FAbilitySystem::End가 먼저 모두 취소한다(OnEnd 호출).
#include "Core/Log.h"
#include "Scene/Ability/AbilitySystem.h"
#include "Scene/Scene.h"
#include "Scripting/LuaRuntime.h"

#include <algorithm>
#include <format>
#include <stdexcept>

E_DECLARE_LOG_CATEGORY(LogScript)

namespace
{
	constexpr const char* ContextChunk = R"(
local Native, Wait, WaitFrames, WaitUntil, IsYieldable, Yield = ...
local Ctx = {}
Ctx.__index = Ctx
function Ctx:Wait(Seconds) return Wait(Seconds) end
function Ctx:WaitFrames(Count) return WaitFrames(Count) end
function Ctx:WaitUntil(Predicate) return WaitUntil(Predicate) end
function Ctx:WaitAnimNotify(Name, Timeout)
	local Start, Hit = Native.Now(), false
	WaitUntil(function()
		if Native.HasNotify(self.Id, Name) then Hit = true return true end
		return Timeout ~= nil and Native.Now() - Start >= Timeout
	end)
	return Hit
end
function Ctx:PlayMontage(...) return self.Owner:PlayMontage(...) end
function Ctx:ApplyEffectToSelf(Name, Options) return Native.ApplyEffect(self.Id, self.Owner, Name, Options) end
function Ctx:ApplyEffectToTarget(Target, Name, Options) return Native.ApplyEffect(self.Id, Target, Name, Options) end
function Ctx:CommitAbility() return Native.Commit(self.Id) end
Ctx.CommitCost = Ctx.CommitAbility
Ctx.CommitCooldown = Ctx.CommitAbility
function Ctx:EndAbility()
	Native.End(self.Id)
	if IsYieldable() then Yield() end -- 끝낸 능력의 코루틴은 더 진행하지 않는다
end
function Ctx:IsActive() return Native.IsActive(self.Id) end
function Ctx:HasAuthority() return self.Authority end
function Ctx:IsPredicting() return self.Predicted end
function Ctx:IsLocallyControlled() return Native.IsLocallyControlled(self.Id) end
function Ctx:GetAttribute(Name) return self.Owner:GetAttribute(Name) end
local function Runner(Self, Context) return Self:OnActivate(Context) end
return Ctx, Runner
)";
} // namespace

void FLuaRuntime::RegisterAbilityBindings()
{
	const auto System = [this]() -> FAbilitySystem* { return AbilitySystem != nullptr && AbilitySystem->IsPlaying() ? AbilitySystem : nullptr; };
	const auto ToParams = [](const sol::optional<sol::table>& Options) {
		FEffectApplyParams Params;
		if (!Options)
		{
			return Params;
		}
		if (const sol::optional<FScriptEntity> Source = (*Options)["Source"]; Source)
		{
			Params.Source = Source->Entity;
		}
		if (const sol::optional<float> Level = (*Options)["Level"]; Level)
		{
			Params.Level = *Level;
		}
		if (const sol::optional<sol::table> SetByCaller = (*Options)["SetByCaller"]; SetByCaller)
		{
			SetByCaller->for_each([&](const sol::object& Key, const sol::object& Value) {
				if (Key.is<std::string>() && Value.is<double>())
				{
					Params.SetByCaller[Key.as<std::string>()] = static_cast<float>(Value.as<double>());
				}
			});
		}
		return Params;
	};

	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["HasAbilitySystem"] = [System](const FScriptEntity& Entity) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->Find(Entity.Entity) != nullptr;
	};
	EntityType["TryActivateAbility"] = [System](const FScriptEntity& Entity, const std::string& Name) {
		FAbilitySystem* Abilities = System();
		if (Abilities == nullptr)
		{
			return std::make_tuple(false, std::string("NoAbilitySystem"));
		}
		const FAbilityActivateResult Result = Abilities->TryActivateAbility(Entity.Entity, Name);
		return std::make_tuple(Result.bActivated, Result.Reason);
	};
	EntityType["CancelAbility"] = [System](const FScriptEntity& Entity, const std::string& Name) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->CancelAbility(Entity.Entity, Name);
	};
	EntityType["IsAbilityActive"] = [System](const FScriptEntity& Entity, const std::string& Name) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->IsAbilityActive(Entity.Entity, Name);
	};
	EntityType["GetAbilityCooldown"] = [System](const FScriptEntity& Entity, const std::string& Name) {
		FAbilitySystem* Abilities = System();
		float           Duration  = 0.0f;
		const float     Remaining = Abilities != nullptr ? Abilities->GetCooldownRemaining(Entity.Entity, Name, &Duration) : 0.0f;
		return std::make_tuple(Remaining, Duration);
	};
	EntityType["GetAbilities"] = [System, this](const FScriptEntity& Entity) {
		sol::table      Result    = Lua.create_table();
		FAbilitySystem* Abilities = System();
		const std::vector<std::string>* Granted = Abilities != nullptr ? Abilities->GetGrantedAbilities(Entity.Entity) : nullptr;
		for (size_t Index = 0; Granted != nullptr && Index < Granted->size(); ++Index)
		{
			Result[Index + 1] = (*Granted)[Index];
		}
		return Result;
	};
	EntityType["GiveAbility"] = [System](const FScriptEntity& Entity, const std::string& Name) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->GiveAbility(Entity.Entity, Name);
	};
	EntityType["RemoveAbility"] = [System](const FScriptEntity& Entity, const std::string& Name) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->RemoveAbility(Entity.Entity, Name);
	};
	const auto GetAttribute = [System](const FScriptEntity& Entity, const std::string& Name, bool bBase) -> sol::optional<float> {
		FAbilitySystem* Abilities = System();
		float           Value     = 0.0f;
		if (Abilities != nullptr && Abilities->GetAttribute(Entity.Entity, Name, Value, bBase))
		{
			return Value;
		}
		return sol::nullopt;
	};
	EntityType["GetAttribute"]     = [GetAttribute](const FScriptEntity& Entity, const std::string& Name) { return GetAttribute(Entity, Name, false); };
	EntityType["GetBaseAttribute"] = [GetAttribute](const FScriptEntity& Entity, const std::string& Name) { return GetAttribute(Entity, Name, true); };
	EntityType["SetBaseAttribute"] = [System](const FScriptEntity& Entity, const std::string& Name, float Value) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->SetBaseAttribute(Entity.Entity, Name, Value);
	};
	EntityType["HasTag"] = [System](const FScriptEntity& Entity, const std::string& Tag) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->HasTag(Entity.Entity, Tag);
	};
	EntityType["HasTagExact"] = [System](const FScriptEntity& Entity, const std::string& Tag) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->HasTagExact(Entity.Entity, Tag);
	};
	EntityType["GetTagCount"] = [System](const FScriptEntity& Entity, const std::string& Tag) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr ? Abilities->GetTagCount(Entity.Entity, Tag) : 0;
	};
	EntityType["AddLooseTag"] = [System](const FScriptEntity& Entity, const std::string& Tag, sol::optional<int32> Count) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->AddLooseTag(Entity.Entity, Tag, Count.value_or(1));
	};
	EntityType["RemoveLooseTag"] = [System](const FScriptEntity& Entity, const std::string& Tag, sol::optional<int32> Count) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->RemoveLooseTag(Entity.Entity, Tag, Count.value_or(1));
	};
	EntityType["ApplyEffect"] = [System, ToParams](const FScriptEntity& Entity, const std::string& Name, sol::optional<sol::table> Options) {
		FAbilitySystem* Abilities = System();
		if (Abilities == nullptr)
		{
			return std::make_tuple(false, 0u);
		}
		const FEffectApplyResult Result = Abilities->ApplyEffect(Entity.Entity, Name, ToParams(Options));
		return std::make_tuple(Result.bApplied, Result.Handle);
	};
	EntityType["RemoveEffect"] = [System](const FScriptEntity& Entity, uint32 Handle) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->RemoveEffect(Entity.Entity, Handle);
	};
	EntityType["RemoveEffectsWithTags"] = [System](const FScriptEntity& Entity, const sol::object& Tags) {
		FAbilitySystem*          Abilities = System();
		std::vector<std::string> List;
		if (Tags.is<std::string>())
		{
			List.push_back(Tags.as<std::string>());
		}
		else if (Tags.get_type() == sol::type::table)
		{
			Tags.as<sol::table>().for_each([&](const sol::object&, const sol::object& Value) {
				if (Value.is<std::string>())
				{
					List.push_back(Value.as<std::string>());
				}
			});
		}
		return Abilities != nullptr ? Abilities->RemoveEffectsWithTags(Entity.Entity, List) : 0;
	};
	EntityType["GetActiveEffects"] = [System, this](const FScriptEntity& Entity) {
		sol::table      Result    = Lua.create_table();
		FAbilitySystem* Abilities = System();
		int32           Index     = 1;
		for (const FActiveEffectView& Effect : Abilities != nullptr ? Abilities->GetActiveEffects(Entity.Entity) : std::vector<FActiveEffectView>())
		{
			sol::table Item   = Lua.create_table();
			Item["Name"]      = Effect.Name;
			Item["Handle"]    = Effect.Handle;
			Item["Stacks"]    = Effect.Stacks;
			Item["Remaining"] = Effect.Remaining;
			Item["Duration"]  = Effect.Duration;
			Item["Predicted"] = Effect.bPredicted;
			Result[Index++]   = Item;
		}
		return Result;
	};

	// ---- Abilities 테이블
	sol::table AbilitiesTable       = Lua.create_named_table("Abilities");
	AbilitiesTable["FindInRadius"] = [System, this](const FVector3& Center, float Radius, sol::optional<FScriptEntity> Ignore) {
		sol::table      Result    = Lua.create_table();
		FAbilitySystem* Abilities = System();
		if (Abilities == nullptr || Scene == nullptr)
		{
			return Result;
		}
		std::vector<std::pair<float, FEntity>> Found;
		for (const FEntity Entity : Abilities->GetEntities())
		{
			if ((Ignore && Ignore->Entity == Entity) || !Scene->GetRegistry().Has<FTransformComponent>(Entity))
			{
				continue;
			}
			const float Distance = (Scene->GetTransform(Entity).GetWorldPosition() - Center).Length();
			if (Distance <= Radius)
			{
				Found.emplace_back(Distance, Entity);
			}
		}
		std::sort(Found.begin(), Found.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
		for (size_t Index = 0; Index < Found.size(); ++Index)
		{
			Result[Index + 1] = FScriptEntity{ Found[Index].second };
		}
		return Result;
	};
	AbilitiesTable["FindLocal"] = [System, this]() -> sol::object {
		FAbilitySystem* Abilities = System();
		if (Abilities == nullptr)
		{
			return sol::lua_nil;
		}
		const int32 Local   = GetLocalPlayerId();
		const bool  bServer = NetHooks == nullptr || NetHooks->bIsServer;
		for (const FEntity Entity : Abilities->GetEntities())
		{
			const FAbilitySystemComponent* Component = Abilities->Find(Entity);
			const int32                    Owner     = GetOwner(Entity);
			if (Component != nullptr && Component->bAcceptInput && ((Owner >= 0 && Owner == Local) || (Owner < 0 && bServer)))
			{
				return sol::make_object(Lua, FScriptEntity{ Entity });
			}
		}
		return sol::lua_nil;
	};
	AbilitiesTable["IsAutoCast"] = []() { return FAbilitySystem::IsAutoCastEnabled(); };
	// 마지막 발동 실패: 능력, 이유, 지난 초 (없으면 nil)
	EntityType["GetLastAbilityFailure"] = [System](const FScriptEntity& Entity) -> std::tuple<sol::optional<std::string>, sol::optional<std::string>, sol::optional<double>> {
		FAbilitySystem*                Abilities = System();
		const FAbilitySystemComponent* Component = Abilities != nullptr ? Abilities->Find(Entity.Entity) : nullptr;
		if (Component == nullptr || Component->Runtime.LastFailedTime < 0.0)
		{
			return { sol::nullopt, sol::nullopt, sol::nullopt };
		}
		return { Component->Runtime.LastFailedAbility, Component->Runtime.LastFailedReason, Abilities->GetClock() - Component->Runtime.LastFailedTime };
	};
	AbilitiesTable["Describe"] = [System](const FScriptEntity& Entity) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr ? Abilities->Describe(Entity.Entity) : std::string();
	};

	// ---- 능력 ctx (Lua로 정의, 네이티브 함수는 _AbilityNative)
	sol::table Native         = Lua.create_table();
	Native["Now"]             = [this]() { return AbilityClock; };
	Native["HasNotify"]       = [System, this](uint32 Id, const std::string& Name) {
		const auto      Task      = AbilityTasks.find(Id);
		FAbilitySystem* Abilities = System();
		return Task != AbilityTasks.end() && Abilities != nullptr && Abilities->HasAnimNotify(Task->second.Owner, Name);
	};
	Native["ApplyEffect"] = [System, ToParams](uint32 Id, const FScriptEntity& Target, const std::string& Name, sol::optional<sol::table> Options) {
		FAbilitySystem* Abilities = System();
		if (Abilities == nullptr)
		{
			return std::make_tuple(false, 0u);
		}
		const FEffectApplyResult Result = Abilities->ApplyEffectFromAbility(Id, Target.Entity, Name, ToParams(Options));
		return std::make_tuple(Result.bApplied, Result.Handle);
	};
	Native["Commit"] = [System](uint32 Id) {
		FAbilitySystem* Abilities = System();
		return Abilities != nullptr && Abilities->CommitAbility(Id);
	};
	Native["End"] = [System](uint32 Id) {
		if (FAbilitySystem* Abilities = System())
		{
			Abilities->EndAbility(Id, false);
		}
	};
	Native["IsActive"] = [System](uint32 Id) {
		FAbilitySystem* Abilities = System();
		const FAbilityInstance* Instance = Abilities != nullptr ? Abilities->FindInstance(Id) : nullptr;
		return Instance != nullptr && !Instance->bEnding;
	};
	Native["IsLocallyControlled"] = [this](uint32 Id) {
		const auto Task = AbilityTasks.find(Id);
		if (Task == AbilityTasks.end())
		{
			return false;
		}
		const int32 Owner = GetOwner(Task->second.Owner);
		return Owner >= 0 ? Owner == GetLocalPlayerId() : (NetHooks == nullptr || NetHooks->bIsServer);
	};
	sol::load_result Chunk = Lua.load(ContextChunk, "=AbilityContext");
	E_CHECKF(Chunk.valid(), "능력 ctx 정의 청크 로드 실패");
	sol::protected_function        Define = Chunk.get<sol::protected_function>();
	sol::protected_function_result Result = Define(Native, Lua["Wait"], Lua["WaitFrames"], Lua["WaitUntil"], Lua["coroutine"]["isyieldable"], Lua["coroutine"]["yield"]);
	E_CHECKF(Result.valid() && Result.return_count() == 2, "능력 ctx 정의 청크 실행 실패");
	AbilityContextMeta = Result.get<sol::table>(0);
	AbilityRunner      = sol::protected_function(Result.get<sol::function>(1), Traceback);
}

// ---------------------------------------------------------------- 능력 스크립트 호스트

bool FLuaRuntime::StartAbility(const FAbilityScriptStart& Start)
{
	if (Scene == nullptr || AbilityTasks.contains(Start.InstanceId))
	{
		return false;
	}
	FScriptClass& Class = LoadClass(Start.Script);
	if (!Class.bValid)
	{
		return false; // 로드 오류는 LoadClass가 한 번 보고
	}
	if (Class.Class["OnActivate"].get_type() != sol::type::function)
	{
		ReportError(std::format("능력 스크립트 {}: OnActivate(ctx)가 없습니다", Class.AssetName));
		return false;
	}
	FAbilityTask Task;
	Task.Id     = Start.InstanceId;
	Task.Owner  = Start.Owner;
	Task.Script = Start.Script;
	Task.Self   = Lua.create_table();
	Task.Self["entity"]           = FScriptEntity{ Start.Owner };
	Task.Self["Properties"]       = MakeProperties(Class, Start.Script, std::string());
	Task.Self[sol::metatable_key] = Class.Metatable;
	Task.Context                  = Lua.create_table();
	Task.Context["Id"]            = Start.InstanceId;
	Task.Context["Owner"]         = FScriptEntity{ Start.Owner };
	Task.Context["Ability"]       = Start.Ability;
	Task.Context["Level"]         = Start.Level;
	Task.Context["Authority"]     = Start.bAuthority;
	Task.Context["Predicted"]     = Start.bPredicted;
	Task.Context[sol::metatable_key] = AbilityContextMeta;
	sol::protected_function_result Created = CoroutineCreate(AbilityRunner);
	if (!Created.valid())
	{
		return false;
	}
	Task.Coroutine.Thread = Created.get<sol::object>();
	AbilityTasks.emplace(Start.InstanceId, std::move(Task));
	ResumeAbility(Start.InstanceId, true);
	return true;
}

void FLuaRuntime::ResumeAbility(uint32 Id, bool bFirst)
{
	auto Found = AbilityTasks.find(Id);
	if (Found == AbilityTasks.end())
	{
		return;
	}
	FAbilityTask& Task = Found->second;
	if (!bFirst && Task.Coroutine.Wait == EScriptWait::Until)
	{
		sol::protected_function_result Check = Task.Coroutine.Predicate();
		if (Check.valid())
		{
			const bool bReady = Check.return_count() > 0 && Check.get_type(0) != sol::type::lua_nil &&
			                    !(Check.get_type(0) == sol::type::boolean && !Check.get<bool>(0));
			if (!bReady)
			{
				return;
			}
		}
		else
		{
			const sol::error Error = Check;
			ReportError(std::format("능력 스크립트 오류 ({}:WaitUntil) — 능력을 취소합니다\n{}", Task.Script, Error.what()));
			FinishAbility(Id, true);
			if (AbilitySystem != nullptr)
			{
				AbilitySystem->EndAbility(Id, true);
			}
			return;
		}
	}

	FScriptCoroutine Coroutine = Task.Coroutine; // 재개 중 맵이 바뀔 수 있다
	const FEntity    Owner     = Task.Owner;
	std::vector<sol::object> Args;
	if (bFirst)
	{
		Args = { Task.Self, Task.Context };
	}
	Task.bResuming = true;
	std::string Error;
	bool        bWaiting = false;
	{
		const FInstanceScope Scope(*this, Owner);
		bWaiting = ResumeCoroutine(Coroutine, Args, Error);
	}
	Found = AbilityTasks.find(Id);
	if (Found == AbilityTasks.end())
	{
		return;
	}
	Found->second.bResuming = false;
	Found->second.Coroutine = Coroutine;
	Found->second.Coroutine.YieldFrame = bFirst ? AbilityTicks + 1 : AbilityTicks; // 시작/재개한 틱에는 다시 세지 않는다
	if (bFirst && AbilitySystem != nullptr)
	{
		AbilitySystem->EndPredictionWindow(Id); // 첫 대기(또는 끝) — 이후 자기 효과는 예측하지 않는다
	}
	if (Found->second.bStopRequested)
	{
		FinishAbility(Id, Found->second.bStopCancelled);
		return;
	}
	if (!Error.empty())
	{
		ReportError(std::format("능력 스크립트 오류 ({}:OnActivate) — 능력을 취소합니다\n{}", Found->second.Script, Error));
		if (AbilitySystem != nullptr)
		{
			AbilitySystem->EndAbility(Id, true); // → StopAbility → FinishAbility
		}
		FinishAbility(Id, true);
		return;
	}
	if (!bWaiting)
	{
		if (AbilitySystem != nullptr)
		{
			AbilitySystem->EndAbility(Id, false); // 반환 = 끝
		}
		FinishAbility(Id, false);
	}
}

void FLuaRuntime::StopAbility(uint32 InstanceId, bool bCancelled)
{
	const auto Found = AbilityTasks.find(InstanceId);
	if (Found == AbilityTasks.end())
	{
		return;
	}
	if (Found->second.bResuming)
	{
		Found->second.bStopRequested = true; // 재개가 끝난 뒤 정리 (ctx:EndAbility 안에서)
		Found->second.bStopCancelled = bCancelled;
		return;
	}
	FinishAbility(InstanceId, bCancelled);
}

void FLuaRuntime::FinishAbility(uint32 Id, bool bCancelled)
{
	const auto Found = AbilityTasks.find(Id);
	if (Found == AbilityTasks.end())
	{
		return;
	}
	const sol::table Self    = Found->second.Self;
	const sol::table Context = Found->second.Context;
	const FEntity    Owner   = Found->second.Owner;
	const std::string Script = Found->second.Script;
	AbilityTasks.erase(Found);
	const sol::object Method = Self["OnEnd"];
	if (Method.get_type() == sol::type::function && Scene != nullptr)
	{
		sol::protected_function        Function(Method.as<sol::function>(), Traceback);
		const FInstanceScope           Scope(*this, Owner);
		sol::protected_function_result Result = Function(Self, Context, bCancelled);
		if (!Result.valid())
		{
			const sol::error Error = Result;
			ReportError(std::format("능력 스크립트 오류 ({}:OnEnd)\n{}", Script, Error.what()));
		}
	}
}

void FLuaRuntime::TickAbilities(float DeltaSeconds)
{
	AbilityClock += DeltaSeconds;
	++AbilityTicks;
	std::vector<uint32> Ready;
	for (auto& [Id, Task] : AbilityTasks)
	{
		FScriptCoroutine& Coroutine = Task.Coroutine;
		if (Coroutine.YieldFrame == AbilityTicks || Task.bResuming)
		{
			continue;
		}
		bool              bReady    = false;
		switch (Coroutine.Wait)
		{
		case EScriptWait::Frames:  bReady = --Coroutine.Frames <= 0; break;
		case EScriptWait::Seconds: Coroutine.Seconds -= DeltaSeconds; bReady = Coroutine.Seconds <= 1.0e-6; break;
		case EScriptWait::Until:   bReady = true; break;
		}
		if (bReady)
		{
			Ready.push_back(Id);
		}
	}
	std::sort(Ready.begin(), Ready.end());
	for (const uint32 Id : Ready)
	{
		ResumeAbility(Id, false);
	}
}

void FLuaRuntime::ClearAbilities()
{
	AbilityTasks.clear();
}

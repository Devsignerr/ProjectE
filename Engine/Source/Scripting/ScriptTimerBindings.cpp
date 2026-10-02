// Lua 타이머/코루틴 대기 (Phase 41-1).
//
//   Timer.After(초, 함수) → 번호        한 번 (만든 프레임부터 세어 '초'가 지난 첫 프레임)
//   Timer.Every(초, 함수) → 번호        반복 (함수가 false를 돌려주면 멈춤, 한 프레임에 최대 한 번 — 밀린 횟수를 몰아 부르지 않는다)
//   Timer.Cancel(번호) → bool          Timer.IsActive(번호) → bool
//   Coroutine.Start(함수, 인자...) → 번호  바로 실행해 첫 대기까지 진행 (Unity StartCoroutine)
//   Coroutine.Stop(번호) → bool        Coroutine.IsRunning(번호) → bool
//   코루틴 안에서만: Wait(초)(없거나 0 = 다음 프레임), WaitFrames(n), WaitUntil(함수) — 매 프레임 확인
//
// 규칙
// - 소유자 = 지금 실행 중인 스크립트 인스턴스 (OnStart/OnUpdate/이벤트/타이머·코루틴 안). 인스턴스 코드 밖(RunString, 비헤이비어 트리
//   스크립트 객체)에서 부르면 오류. 인스턴스가 사라지면(OnDestroy 뒤·엔티티 파괴·스크립트 변경·EndPlay) 함께 사라지고,
//   핫 리로드는 그 스크립트 인스턴스의 타이머/코루틴을 모두 취소한다 (옛 코드 클로저가 남지 않게 — OnStart는 다시 불리지 않는다).
// - 시간 = 스크립트 시간 (FScriptSystem::Update의 DeltaSeconds = Time.DeltaTime, MaxDeltaSeconds로 제한). 에디터 일시정지 중에는
//   Update가 불리지 않으므로 멈춘다. 발사/재개는 Update에서 모든 OnUpdate가 끝난 뒤 (UpdateOrder 순서), 노티파이/UI 이벤트 전.
// - 오류: 타이머 함수/코루틴 오류는 그 인스턴스만 멈춘다 (다른 스크립트 오류와 같음 — 핫 리로드 성공 시 재개, 타이머는 없는 채로).
//   Coroutine.Start의 첫 실행 오류는 부른 쪽 메서드의 오류가 된다.
// - ExecutionLocation: 인스턴스가 이 머신에서 돌 때만 존재하므로 자동으로 따른다.
#include "Scripting/LuaRuntime.h"

#include "Core/Log.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

E_DECLARE_LOG_CATEGORY(LogScript)

namespace
{
	constexpr double TimeEpsilon = 1.0e-6; // 0.1초 × 3 = 0.3 같은 누적 오차로 한 프레임 늦어지지 않게

	// Wait 계열은 Lua로 정의한다 (C 함수에서 yield하지 않도록). Token은 사용자 coroutine.yield와 구분하는 표식
	constexpr const char* WaitChunk = R"(
local Token, IsYieldable, Yield = ...
local function Check(Name)
	if not IsYieldable() then
		error(Name .. "()는 Coroutine.Start로 시작한 코루틴 안에서만 쓸 수 있습니다", 3)
	end
end
function Wait(Seconds)
	Check("Wait")
	if Seconds ~= nil and type(Seconds) ~= "number" then error("Wait(초): 숫자가 필요합니다", 2) end
	return Yield(Token, "Seconds", Seconds or 0)
end
function WaitFrames(Count)
	Check("WaitFrames")
	if Count ~= nil and type(Count) ~= "number" then error("WaitFrames(n): 숫자가 필요합니다", 2) end
	return Yield(Token, "Frames", Count or 1)
end
function WaitUntil(Predicate)
	Check("WaitUntil")
	if type(Predicate) ~= "function" then error("WaitUntil(함수): 함수가 필요합니다", 2) end
	return Yield(Token, "Until", Predicate)
end
)";

	bool IsFalse(const sol::protected_function_result& Result)
	{
		return Result.return_count() > 0 && Result.get_type(0) == sol::type::boolean && !Result.get<bool>(0);
	}
} // namespace

FLuaRuntime::FInstanceScope::FInstanceScope(FLuaRuntime& InRuntime, FEntity Entity)
	: Runtime(InRuntime)
	, Previous(InRuntime.CurrentInstance)
{
	Runtime.CurrentInstance = Entity;
}

FLuaRuntime::FInstanceScope::~FInstanceScope()
{
	Runtime.CurrentInstance = Previous;
}

FLuaRuntime::FScriptInstance& FLuaRuntime::RequireCurrentInstance(const char* ApiName)
{
	const auto Found = CurrentInstance.IsValid() ? Instances.find(CurrentInstance.ToId()) : Instances.end();
	if (Found == Instances.end())
	{
		throw std::runtime_error(std::format("{}: 스크립트 인스턴스 코드(OnStart/OnUpdate/이벤트/타이머/코루틴) 안에서만 쓸 수 있습니다", ApiName));
	}
	return Found->second;
}

void FLuaRuntime::FaultInstance(FScriptInstance& Instance, const std::string& Where, const std::string& Message)
{
	Instance.bFaulted = true;
	ReportError(std::format("스크립트 오류 ({}:{}) — 이 인스턴스는 멈춥니다 (스크립트 저장 시 재개)\n{}", Instance.ScriptAsset, Where, Message));
	ClearInstanceTasks(Instance);
}

void FLuaRuntime::ClearInstanceTasks(FScriptInstance& Instance)
{
	for (const FScriptTimer& Timer : Instance.Timers)
	{
		Tasks.erase(Timer.Id);
	}
	for (const FScriptCoroutine& Coroutine : Instance.Coroutines)
	{
		Tasks.erase(Coroutine.Id);
	}
	Instance.Timers.clear();
	Instance.Coroutines.clear();
}

void FLuaRuntime::RegisterTimerBindings()
{
	CoroutineCreate = Lua["coroutine"]["create"];
	CoroutineResume = Lua["coroutine"]["resume"];
	CoroutineStatus = Lua["coroutine"]["status"];
	WaitToken       = Lua.create_table();
	{
		sol::load_result Chunk = Lua.load(WaitChunk, "=Wait");
		E_CHECKF(Chunk.valid(), "Wait 정의 청크 로드 실패");
		sol::protected_function        Define = Chunk.get<sol::protected_function>();
		sol::protected_function_result Result = Define(WaitToken, Lua["coroutine"]["isyieldable"], Lua["coroutine"]["yield"]);
		E_CHECKF(Result.valid(), "Wait 정의 청크 실행 실패");
	}

	const auto AddTimer = [this](double Seconds, sol::protected_function Callback, bool bRepeat, const char* ApiName) -> uint32 {
		if (!(Seconds >= 0.0)) // NaN 포함
		{
			throw std::runtime_error(std::format("{}: 초는 0 이상이어야 합니다", ApiName));
		}
		if (!Callback.valid() || Callback.get_type() != sol::type::function)
		{
			throw std::runtime_error(std::format("{}: 함수가 필요합니다", ApiName));
		}
		FScriptInstance& Instance = RequireCurrentInstance(ApiName);
		FScriptTimer     Timer;
		Timer.Id           = NextTaskId++;
		Timer.Remaining    = Seconds;
		Timer.Interval     = bRepeat ? Seconds : 0.0;
		Timer.bRepeat      = bRepeat;
		Timer.CreatedFrame = FrameCount;
		Timer.Callback     = sol::protected_function(Callback, Traceback);
		Tasks[Timer.Id]    = { Instance.Entity.ToId(), false };
		Instance.Timers.push_back(std::move(Timer));
		return Instance.Timers.back().Id;
	};

	sol::table TimerTable  = Lua.create_named_table("Timer");
	TimerTable["After"]    = [AddTimer](double Seconds, sol::protected_function Callback) { return AddTimer(Seconds, std::move(Callback), false, "Timer.After"); };
	TimerTable["Every"]    = [AddTimer](double Seconds, sol::protected_function Callback) { return AddTimer(Seconds, std::move(Callback), true, "Timer.Every"); };
	TimerTable["IsActive"] = [this](uint32 Id) {
		const auto Found = Tasks.find(Id);
		return Found != Tasks.end() && !Found->second.bCoroutine;
	};
	TimerTable["Cancel"] = [this](uint32 Id) {
		const auto Found = Tasks.find(Id);
		if (Found == Tasks.end() || Found->second.bCoroutine)
		{
			return false;
		}
		// 벡터 항목은 다음 UpdateTasks 정리에서 지운다 (지금 그 벡터를 순회 중일 수 있다)
		Tasks.erase(Found);
		return true;
	};

	sol::table CoroutineTable  = Lua.create_named_table("Coroutine");
	CoroutineTable["IsRunning"] = [this](uint32 Id) {
		const auto Found = Tasks.find(Id);
		return Found != Tasks.end() && Found->second.bCoroutine;
	};
	CoroutineTable["Stop"] = [this](uint32 Id) {
		const auto Found = Tasks.find(Id);
		if (Found == Tasks.end() || !Found->second.bCoroutine)
		{
			return false;
		}
		Tasks.erase(Found); // 자기 자신을 멈춰도 안전: 재개가 끝나면 목록에서 빠진다
		return true;
	};
	CoroutineTable["Start"] = [this](sol::protected_function Function, sol::variadic_args Args) -> uint32 {
		if (!Function.valid() || Function.get_type() != sol::type::function)
		{
			throw std::runtime_error("Coroutine.Start: 함수가 필요합니다");
		}
		FScriptInstance&               Instance = RequireCurrentInstance("Coroutine.Start");
		const uint64                   OwnerId  = Instance.Entity.ToId();
		sol::protected_function_result Created  = CoroutineCreate(Function);
		if (!Created.valid())
		{
			throw std::runtime_error("Coroutine.Start: 코루틴을 만들 수 없습니다");
		}
		FScriptCoroutine Coroutine;
		Coroutine.Id     = NextTaskId++;
		Coroutine.Thread = Created.get<sol::object>();
		Tasks[Coroutine.Id] = { OwnerId, true };

		std::vector<sol::object> Values(Args.begin(), Args.end());
		std::string              Error;
		const bool               bWaiting = ResumeCoroutine(Coroutine, Values, Error);
		const uint32             Id       = Coroutine.Id;
		if (!bWaiting || !Tasks.contains(Id))
		{
			Tasks.erase(Id);
			if (!Error.empty())
			{
				throw std::runtime_error(std::format("Coroutine.Start: 코루틴 오류\n{}", Error));
			}
			return Id; // 대기 없이 끝났거나 스스로 멈춤
		}
		// 첫 실행 중에 인스턴스 맵은 바뀌지 않지만(생성은 Update 2단계뿐) 안전하게 다시 찾는다
		const auto Owner = Instances.find(OwnerId);
		if (Owner == Instances.end())
		{
			Tasks.erase(Id);
			return Id;
		}
		Owner->second.Coroutines.push_back(std::move(Coroutine));
		return Id;
	};
}

bool FLuaRuntime::ResumeCoroutine(FScriptCoroutine& Coroutine, const std::vector<sol::object>& Args, std::string& OutError)
{
	sol::protected_function_result Result = CoroutineResume(Coroutine.Thread, sol::as_args(Args));
	if (!Result.valid())
	{
		const sol::error Error = Result;
		OutError               = Error.what();
		return false;
	}
	if (Result.return_count() == 0 || Result.get_type(0) != sol::type::boolean || !Result.get<bool>(0))
	{
		// resume이 false, 메시지 → 코루틴 쪽 콜스택을 붙인다
		const sol::object       Message      = Result.return_count() > 1 ? Result.get<sol::object>(1) : sol::object(sol::lua_nil);
		sol::protected_function TracebackFn  = Lua["debug"]["traceback"];
		sol::protected_function_result Trace = TracebackFn.valid() ? TracebackFn(Coroutine.Thread, Message) : sol::protected_function_result();
		OutError = Trace.valid() && Trace.return_count() > 0 && Trace.get_type(0) == sol::type::string
		               ? Trace.get<std::string>(0)
		               : (Message.is<std::string>() ? Message.as<std::string>() : std::string("(알 수 없는 오류)"));
		return false;
	}

	sol::protected_function_result Status = CoroutineStatus(Coroutine.Thread);
	if (!Status.valid() || Status.get<std::string>(0) != "suspended")
	{
		return false; // 끝남
	}

	// 대기 종류: Wait 계열이면 (true, Token, 종류, 값), 사용자 yield면 다음 프레임
	Coroutine.YieldFrame = FrameCount;
	Coroutine.Predicate  = sol::protected_function();
	Coroutine.Wait       = EScriptWait::Frames;
	Coroutine.Frames     = 1;
	if (Result.return_count() >= 4 && Result.get_type(1) == sol::type::table && Result.get<sol::table>(1) == WaitToken)
	{
		const std::string Kind = Result.get<std::string>(2);
		if (Kind == "Seconds")
		{
			const double Seconds = Result.get<double>(3);
			if (Seconds > TimeEpsilon)
			{
				Coroutine.Wait    = EScriptWait::Seconds;
				Coroutine.Seconds = Seconds;
			}
		}
		else if (Kind == "Frames")
		{
			Coroutine.Frames = std::max(1, static_cast<int32>(Result.get<double>(3)));
		}
		else if (Kind == "Until")
		{
			Coroutine.Wait      = EScriptWait::Until;
			Coroutine.Predicate = sol::protected_function(Result.get<sol::function>(3), Traceback);
		}
	}
	return true;
}

void FLuaRuntime::UpdateTasks(float DeltaSeconds, const FInput* InInput)
{
	if (Tasks.empty())
	{
		return;
	}
	FRegistry& Registry = Scene->GetRegistry();
	struct FDueTimer
	{
		uint32                  Id = 0;
		sol::protected_function Callback;
	};
	std::vector<FDueTimer> DueTimers;
	std::vector<uint32>    ReadyCoroutines;

	for (const FEntity Entity : UpdateOrder)
	{
		const uint64 OwnerId = Entity.ToId();
		auto         Found   = Instances.find(OwnerId);
		if (Found == Instances.end() || (Found->second.Timers.empty() && Found->second.Coroutines.empty()))
		{
			continue;
		}
		if (Found->second.bFaulted || !Found->second.bStarted || !Registry.IsValid(Entity))
		{
			ClearInstanceTasks(Found->second);
			continue;
		}

		// 1. 시간 진행 + 발사할 타이머 모으기 (콜백이 타이머를 더하거나 취소해도 안전하도록 먼저 모은다)
		DueTimers.clear();
		for (FScriptTimer& Timer : Found->second.Timers)
		{
			if (!Tasks.contains(Timer.Id) || Timer.CreatedFrame == FrameCount)
			{
				continue;
			}
			Timer.Remaining -= DeltaSeconds;
			if (Timer.Remaining > TimeEpsilon)
			{
				continue;
			}
			DueTimers.push_back({ Timer.Id, Timer.Callback });
			if (Timer.bRepeat)
			{
				Timer.Remaining += Timer.Interval;
				if (Timer.Remaining <= TimeEpsilon)
				{
					Timer.Remaining = Timer.Interval; // 한 프레임에 한 번만
				}
			}
		}

		// 2. 깨울 코루틴 모으기
		ReadyCoroutines.clear();
		for (FScriptCoroutine& Coroutine : Found->second.Coroutines)
		{
			if (!Tasks.contains(Coroutine.Id) || Coroutine.YieldFrame == FrameCount)
			{
				continue;
			}
			bool bReady = false;
			switch (Coroutine.Wait)
			{
			case EScriptWait::Frames:  bReady = --Coroutine.Frames <= 0; break;
			case EScriptWait::Seconds: Coroutine.Seconds -= DeltaSeconds; bReady = Coroutine.Seconds <= TimeEpsilon; break;
			case EScriptWait::Until:   bReady = true; break; // 조건은 재개 단계에서 (Lua 호출)
			}
			if (bReady)
			{
				ReadyCoroutines.push_back(Coroutine.Id);
			}
		}

		Input = NetHooks != nullptr && NetHooks->ResolveInput ? NetHooks->ResolveInput(Entity, InInput) : InInput;
		const FInstanceScope Scope(*this, Entity);

		// 3. 타이머 발사
		for (FDueTimer& Due : DueTimers)
		{
			Found = Instances.find(OwnerId);
			if (Found == Instances.end() || Found->second.bFaulted)
			{
				break;
			}
			const auto Task = Tasks.find(Due.Id);
			if (Task == Tasks.end())
			{
				continue; // 앞의 콜백이 취소
			}
			const auto Timer   = std::find_if(Found->second.Timers.begin(), Found->second.Timers.end(), [&](const FScriptTimer& Item) { return Item.Id == Due.Id; });
			const bool bRepeat = Timer != Found->second.Timers.end() && Timer->bRepeat;
			if (!bRepeat)
			{
				Tasks.erase(Task); // 한 번짜리는 부르기 전에 끝낸 것으로 (콜백 안 IsActive = false)
			}
			sol::protected_function_result Result = Due.Callback();
			Found = Instances.find(OwnerId);
			if (Found == Instances.end())
			{
				break;
			}
			if (!Result.valid())
			{
				const sol::error Error = Result;
				FaultInstance(Found->second, bRepeat ? "Timer.Every" : "Timer.After", Error.what());
				break;
			}
			if (bRepeat && IsFalse(Result))
			{
				Tasks.erase(Due.Id);
			}
		}

		// 4. 코루틴 재개
		for (const uint32 Id : ReadyCoroutines)
		{
			Found = Instances.find(OwnerId);
			if (Found == Instances.end() || Found->second.bFaulted)
			{
				break;
			}
			if (!Tasks.contains(Id))
			{
				continue;
			}
			auto Item = std::find_if(Found->second.Coroutines.begin(), Found->second.Coroutines.end(), [&](const FScriptCoroutine& C) { return C.Id == Id; });
			if (Item == Found->second.Coroutines.end())
			{
				continue;
			}
			// 재개 중 Coroutine.Start가 벡터를 늘릴 수 있으므로 항목을 꺼내 두고 다시 넣는다
			FScriptCoroutine Coroutine = std::move(*Item);
			Found->second.Coroutines.erase(Item);
			std::string Error;
			bool        bWaiting = true;
			bool        bResume  = true;
			if (Coroutine.Wait == EScriptWait::Until)
			{
				sol::protected_function_result Check = Coroutine.Predicate();
				if (!Check.valid())
				{
					const sol::error CheckError = Check;
					Error    = CheckError.what();
					bWaiting = false;
					bResume  = false;
				}
				else
				{
					bResume = Check.return_count() > 0 && Check.get<sol::object>(0).valid() && Check.get_type(0) != sol::type::lua_nil && !IsFalse(Check);
				}
			}
			if (bResume)
			{
				bWaiting = ResumeCoroutine(Coroutine, {}, Error);
			}
			Found = Instances.find(OwnerId);
			if (Found == Instances.end())
			{
				Tasks.erase(Id);
				break;
			}
			if (!Error.empty())
			{
				Tasks.erase(Id);
				FaultInstance(Found->second, "Coroutine", Error);
				break;
			}
			if (bWaiting && Tasks.contains(Id))
			{
				Found->second.Coroutines.push_back(std::move(Coroutine));
			}
			else
			{
				Tasks.erase(Id);
			}
		}

		// 5. 끝났거나 취소된 항목 정리
		Found = Instances.find(OwnerId);
		if (Found != Instances.end())
		{
			std::erase_if(Found->second.Timers, [this](const FScriptTimer& Timer) { return !Tasks.contains(Timer.Id); });
			std::erase_if(Found->second.Coroutines, [this](const FScriptCoroutine& Coroutine) { return !Tasks.contains(Coroutine.Id); });
		}
	}
	Input = InInput;
}

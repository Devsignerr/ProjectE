#include "Scripting/ScriptDebugger.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Scripting/SolInclude.h" // lua.h / lauxlib.h (Lua는 C++로 컴파일 — lua_error는 C++ 예외)

#include <algorithm>
#include <cstring>
#include <format>

E_DECLARE_LOG_CATEGORY(LogScript)

namespace
{
	// 레지스트리 키 (주소만 쓴다)
	char GDebuggerKey = 0; // 연결된 FScriptDebugger*
	char GThreadsKey  = 0; // 약한 키 표: 이 상태에서 만든 코루틴 스레드
	char GCachedFunctionKey = 0; // 줄 훅 함수 캐시가 가리키는 함수 (수거 방지)

	constexpr int32  MaxStackFrames   = 64;
	constexpr int32  MaxChildren      = 256;  // 테이블 펼치기 한 번에 보이는 항목 수
	constexpr int32  MaxExpandDepth   = 32;   // 펼치기 깊이
	constexpr size_t MaxHandles       = 8192; // 정지 한 번에 발급하는 핸들 수
	constexpr size_t MaxValueLength   = 200;  // 표시 문자열 바이트
	constexpr int32  MaxCountedEntries = 10000; // 테이블 항목 수 셀 때 상한

	int32 StackDepth(lua_State* L)
	{
		lua_Debug Ar;
		int32     Depth = 0;
		while (lua_getstack(L, Depth, &Ar))
		{
			++Depth;
		}
		return Depth;
	}

	// UTF-8 글자 중간에서 자르지 않는다
	std::string Truncate(std::string Text)
	{
		if (Text.size() <= MaxValueLength)
		{
			return Text;
		}
		size_t Cut = MaxValueLength;
		while (Cut > 0 && (static_cast<unsigned char>(Text[Cut]) & 0xC0) == 0x80)
		{
			--Cut;
		}
		Text.resize(Cut);
		return Text + "…";
	}

	std::string QuoteString(const char* Data, size_t Length)
	{
		std::string Result = "\"";
		for (size_t Index = 0; Index < Length && Result.size() < MaxValueLength + 8; ++Index)
		{
			const char Char = Data[Index];
			switch (Char)
			{
			case '\n': Result += "\\n"; break;
			case '\r': Result += "\\r"; break;
			case '\t': Result += "\\t"; break;
			case '"':  Result += "\\\""; break;
			case '\\': Result += "\\\\"; break;
			case '\0': Result += "\\0"; break;
			default:   Result += Char; break;
			}
		}
		Result = Truncate(std::move(Result));
		if (Result.back() != '"' || Result.size() == 1)
		{
			Result += '"';
		}
		return Result;
	}

	// luaL_tolstring을 보호 호출로 (__tostring 메타메서드 오류가 디버거 밖으로 나가지 않게)
	int ToStringThunk(lua_State* L)
	{
		luaL_tolstring(L, 1, nullptr);
		return 1;
	}

	std::string ProtectedToString(lua_State* W, int Index)
	{
		Index = lua_absindex(W, Index);
		lua_pushcfunction(W, &ToStringThunk);
		lua_pushvalue(W, Index);
		std::string Result;
		if (lua_pcall(W, 1, 1, 0) == LUA_OK)
		{
			size_t      Length = 0;
			const char* Text   = lua_tolstring(W, -1, &Length);
			Result             = Text != nullptr ? std::string(Text, Length) : std::string("?");
		}
		else
		{
			Result = "(tostring 오류)";
		}
		lua_pop(W, 1);
		return Truncate(std::move(Result));
	}

	// 숫자/불/nil 값 문자열 (값 변환 없이 사본으로)
	std::string ScalarToString(lua_State* W, int Index)
	{
		switch (lua_type(W, Index))
		{
		case LUA_TNIL:     return "nil";
		case LUA_TBOOLEAN: return lua_toboolean(W, Index) ? "true" : "false";
		case LUA_TNUMBER:
		{
			lua_pushvalue(W, Index);
			const char* Text = lua_tostring(W, -1);
			std::string Result = Text != nullptr ? Text : "?";
			lua_pop(W, 1);
			return Result;
		}
		default: return "?";
		}
	}

	// 함수 위치 "<파일:줄>" (스택 맨 위 함수, 그대로 둔다)
	std::string DescribeFunction(lua_State* W)
	{
		lua_Debug Ar;
		lua_pushvalue(W, -1);
		if (!lua_getinfo(W, ">S", &Ar))
		{
			return "function";
		}
		if (Ar.what != nullptr && std::strcmp(Ar.what, "C") == 0)
		{
			return "function [C]";
		}
		const char* Source = Ar.source != nullptr && Ar.source[0] == '@' ? Ar.source + 1 : Ar.short_src;
		return std::format("function <{}:{}>", Source, Ar.linedefined);
	}

	// 표 항목 키 → 표시 이름 (키는 Index에, 값 변환 없이)
	std::string KeyToName(lua_State* W, int Index, int32& OutKind, lua_Integer& OutInteger, std::string& OutString)
	{
		switch (lua_type(W, Index))
		{
		case LUA_TSTRING:
		{
			size_t      Length = 0;
			const char* Text   = lua_tolstring(W, Index, &Length); // 문자열 키는 변환되지 않는다
			OutKind            = 1;
			OutString.assign(Text, Length);
			return Truncate(OutString);
		}
		case LUA_TNUMBER:
			if (lua_isinteger(W, Index))
			{
				OutKind    = 0;
				OutInteger = lua_tointeger(W, Index);
				return std::format("[{}]", OutInteger);
			}
			OutKind = 2;
			return "[" + ScalarToString(W, Index) + "]";
		case LUA_TBOOLEAN:
			OutKind = 2;
			return "[" + ScalarToString(W, Index) + "]";
		default:
			OutKind = 2;
			return "[" + ProtectedToString(W, Index) + "]";
		}
	}
} // namespace

const char* ToString(EScriptPauseReason Reason)
{
	switch (Reason)
	{
	case EScriptPauseReason::Breakpoint:   return "중단점";
	case EScriptPauseReason::Step:         return "단계 실행";
	case EScriptPauseReason::PauseRequest: return "일시 정지";
	case EScriptPauseReason::Error:        return "스크립트 오류";
	}
	return "?";
}

FScriptDebugger::FScriptDebugger()  = default;
FScriptDebugger::~FScriptDebugger() = default;

// ---------------------------------------------------------------- 설정 / 중단점

void FScriptDebugger::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		StepMode        = EStepMode::None;
		bPauseRequested = false;
	}
	UpdateHooks(nullptr);
}

void FScriptDebugger::SuspendUntilNextSession()
{
	bSuspended      = true;
	StepMode        = EStepMode::None;
	bPauseRequested = false;
	if (!bPaused)
	{
		UpdateHooks(nullptr);
	}
}

std::string FScriptDebugger::NormalizeFile(std::string_view File)
{
	std::string Result;
	Result.reserve(File.size());
	for (const char Char : File)
	{
		Result += Char == '\\' ? '/' : (Char >= 'A' && Char <= 'Z' ? static_cast<char>(Char - 'A' + 'a') : Char);
	}
	while (Result.starts_with("./"))
	{
		Result.erase(0, 2);
	}
	while (Result.starts_with('/'))
	{
		Result.erase(0, 1);
	}
	return Result;
}

void FScriptDebugger::RebuildIndex()
{
	BreakpointIndex.clear();
	for (size_t Index = 0; Index < Breakpoints.size(); ++Index)
	{
		if (Breakpoints[Index].bEnabled && Breakpoints[Index].Line > 0)
		{
			BreakpointIndex[NormalizeFile(Breakpoints[Index].File)].Indices.push_back(static_cast<int32>(Index));
		}
	}
	CachedFunction = nullptr;
	CachedEntry    = nullptr;
	++BreakpointRevision;
	if (!bPaused)
	{
		UpdateHooks(nullptr, true);
	}
}

void FScriptDebugger::SetBreakpoints(std::vector<FScriptBreakpoint> InBreakpoints)
{
	Breakpoints = std::move(InBreakpoints);
	RebuildIndex();
}

const FScriptBreakpoint* FScriptDebugger::FindBreakpoint(const std::string& File, int32 Line) const
{
	const std::string Key = NormalizeFile(File);
	for (const FScriptBreakpoint& Breakpoint : Breakpoints)
	{
		if (Breakpoint.Line == Line && NormalizeFile(Breakpoint.File) == Key)
		{
			return &Breakpoint;
		}
	}
	return nullptr;
}

void FScriptDebugger::AddBreakpoint(const FScriptBreakpoint& Breakpoint)
{
	if (const FScriptBreakpoint* Existing = FindBreakpoint(Breakpoint.File, Breakpoint.Line))
	{
		*const_cast<FScriptBreakpoint*>(Existing) = Breakpoint;
	}
	else
	{
		Breakpoints.push_back(Breakpoint);
	}
	RebuildIndex();
}

bool FScriptDebugger::RemoveBreakpoint(const std::string& File, int32 Line)
{
	const std::string Key   = NormalizeFile(File);
	const size_t      Count = std::erase_if(Breakpoints, [&](const FScriptBreakpoint& Item) { return Item.Line == Line && NormalizeFile(Item.File) == Key; });
	if (Count > 0)
	{
		RebuildIndex();
	}
	return Count > 0;
}

bool FScriptDebugger::ToggleBreakpoint(const std::string& File, int32 Line)
{
	if (RemoveBreakpoint(File, Line))
	{
		return false;
	}
	FScriptBreakpoint Breakpoint;
	Breakpoint.File = File;
	Breakpoint.Line = Line;
	AddBreakpoint(Breakpoint);
	return true;
}

void FScriptDebugger::SetBreakpointEnabled(const std::string& File, int32 Line, bool bInEnabled)
{
	if (const FScriptBreakpoint* Existing = FindBreakpoint(File, Line); Existing != nullptr && Existing->bEnabled != bInEnabled)
	{
		const_cast<FScriptBreakpoint*>(Existing)->bEnabled = bInEnabled;
		RebuildIndex();
	}
}

void FScriptDebugger::SetBreakpointCondition(const std::string& File, int32 Line, const std::string& Condition)
{
	if (const FScriptBreakpoint* Existing = FindBreakpoint(File, Line); Existing != nullptr && Existing->Condition != Condition)
	{
		const_cast<FScriptBreakpoint*>(Existing)->Condition = Condition;
		RebuildIndex();
	}
}

void FScriptDebugger::ClearBreakpoints()
{
	if (!Breakpoints.empty())
	{
		Breakpoints.clear();
		RebuildIndex();
	}
}

int32 FScriptDebugger::PruneBreakpoints(const std::string& File, int32 LineCount)
{
	const std::string Key   = NormalizeFile(File);
	const size_t      Count = std::erase_if(Breakpoints, [&](const FScriptBreakpoint& Item) { return Item.Line > LineCount && NormalizeFile(Item.File) == Key; });
	if (Count > 0)
	{
		RebuildIndex();
	}
	return static_cast<int32>(Count);
}

// ---------------------------------------------------------------- 실행 제어

void FScriptDebugger::RequestPause()
{
	if (!bEnabled || bSuspended)
	{
		return;
	}
	bPauseRequested = true;
	if (!bPaused)
	{
		UpdateHooks(nullptr);
	}
}

void FScriptDebugger::Continue()
{
	ResumeCommand = EResume::Continue;
}

void FScriptDebugger::StepOver()
{
	ResumeCommand = EResume::Over;
}

void FScriptDebugger::StepInto()
{
	ResumeCommand = EResume::Into;
}

void FScriptDebugger::StepOut()
{
	ResumeCommand = EResume::Out;
}

bool FScriptDebugger::WantsHooks() const
{
	return MainState != nullptr && bEnabled && !bSuspended && !bPaused && (!BreakpointIndex.empty() || StepMode != EStepMode::None || bPauseRequested);
}

void FScriptDebugger::UpdateHooks(lua_State* Work, bool bForce)
{
	if (MainState == nullptr)
	{
		bHooksInstalled = false;
		return;
	}
	const bool bWant = WantsHooks();
	if (!bWant && !bPaused)
	{
		RehitThread = nullptr; // 훅이 없으면 추적할 수 없다 (다음에 걸 때 새로 시작)
	}
	// 중단점만 있으면 "동적" 모드: 호출/반환 이벤트로 지금 함수가 중단점 있는 파일일 때만 그 스레드에 줄 이벤트를 켠다
	// (중단점 없는 스크립트의 반복문은 줄 훅 비용이 없다). 단계 실행·일시 정지 요청은 모든 줄을 봐야 하므로 정적 모드
	const bool bDynamic = bWant && StepMode == EStepMode::None && !bPauseRequested;
	const bool bReturns = StepMode == EStepMode::Over || StepMode == EStepMode::Out || RehitThread != nullptr;
	const int  Mask     = !bWant ? 0 : bDynamic ? (LUA_MASKCALL | LUA_MASKRET) : (LUA_MASKLINE | (bReturns ? LUA_MASKRET : 0));
	if (!bForce && bWant == bHooksInstalled && Mask == HookMask)
	{
		return;
	}
	bHooksInstalled = bWant;
	HookMask        = Mask;
	bDynamicLines   = bDynamic;
	lua_State* W    = Work != nullptr ? Work : MainState;
	ApplyThreadHook(MainState);
	if (lua_checkstack(W, 4))
	{
		lua_rawgetp(W, LUA_REGISTRYINDEX, &GThreadsKey);
		if (lua_istable(W, -1))
		{
			lua_pushnil(W);
			while (lua_next(W, -2) != 0)
			{
				if (lua_State* Thread = lua_tothread(W, -2))
				{
					ApplyThreadHook(Thread);
				}
				lua_pop(W, 1);
			}
		}
		lua_pop(W, 1);
	}
}

void FScriptDebugger::ApplyThreadHook(lua_State* Thread)
{
	if (!bHooksInstalled)
	{
		lua_sethook(Thread, nullptr, 0, 0);
		return;
	}
	bool bLines = !bDynamicLines;
	if (bDynamicLines && lua_checkstack(Thread, 2))
	{
		// 지금 실행 중(또는 멈춘 자리)의 함수 기준. yield한 코루틴은 맨 위가 C(yield)라 꺼 두고, 재개 후 반환 이벤트가 맞춘다
		lua_Debug Ar;
		bLines = lua_getstack(Thread, 0, &Ar) && ResolveFunction(Thread, &Ar) && CachedEntry != nullptr;
	}
	lua_sethook(Thread, &FScriptDebugger::HookThunk, HookMask | (bLines ? LUA_MASKLINE : 0), 0);
}

void FScriptDebugger::SetThreadLines(lua_State* Thread, bool bLines)
{
	const int Mask = HookMask | (bLines ? LUA_MASKLINE : 0);
	if (lua_gethookmask(Thread) != Mask)
	{
		lua_sethook(Thread, &FScriptDebugger::HookThunk, Mask, 0);
	}
}

bool FScriptDebugger::ResolveFunction(lua_State* L, lua_Debug* Ar)
{
	if (!lua_getinfo(L, "f", Ar))
	{
		return false;
	}
	if (lua_topointer(L, -1) != CachedFunction)
	{
		CachedFunction = lua_topointer(L, -1);
		const bool bC  = lua_iscfunction(L, -1) != 0;
		lua_rawsetp(L, LUA_REGISTRYINDEX, &GCachedFunctionKey); // 함수를 꺼내 잡아 둔다 (주소 재사용으로 헷갈리지 않게)
		CachedEntry = nullptr;
		bCachedFile = !bC && lua_getinfo(L, "S", Ar) && Ar->source != nullptr && Ar->source[0] == '@';
		if (bCachedFile)
		{
			const auto Found = BreakpointIndex.find(NormalizeFile(std::string_view(Ar->source + 1)));
			CachedEntry      = Found != BreakpointIndex.end() ? &Found->second : nullptr;
		}
	}
	else
	{
		lua_pop(L, 1);
	}
	return bCachedFile;
}

// ---------------------------------------------------------------- 연결

FScriptDebugger* FScriptDebugger::FromState(lua_State* L)
{
	if (L == nullptr || !lua_checkstack(L, 1))
	{
		return nullptr;
	}
	lua_rawgetp(L, LUA_REGISTRYINDEX, &GDebuggerKey);
	FScriptDebugger* Debugger = static_cast<FScriptDebugger*>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return Debugger;
}

void FScriptDebugger::Attach(lua_State* L)
{
	E_CHECKF(MainState == nullptr, "스크립트 디버거는 Lua 상태 하나에만 연결할 수 있습니다");
	MainState          = L;
	bSuspended         = false;
	bPauseRequested    = false;
	StepMode           = EStepMode::None;
	StepThread         = nullptr;
	bStepFrameReturned = false;
	bHooksInstalled    = false;
	HookMask           = 0;
	CachedFunction      = nullptr;
	CachedEntry         = nullptr;
	RehitThread         = nullptr;

	*static_cast<FScriptDebugger**>(lua_getextraspace(L)) = this; // 줄 훅 빠른 경로 (이후 코루틴은 복사받는다)
	lua_pushlightuserdata(L, this);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &GDebuggerKey);

	// 코루틴 스레드 기록 표 (약한 키 — 기록이 스레드 수명을 늘리지 않는다)
	lua_newtable(L);
	lua_newtable(L);
	lua_pushliteral(L, "k");
	lua_setfield(L, -2, "__mode");
	lua_setmetatable(L, -2);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &GThreadsKey);

	lua_getglobal(L, "coroutine");
	if (lua_istable(L, -1))
	{
		lua_pushcfunction(L, &FScriptDebugger::CoroutineCreateThunk);
		lua_setfield(L, -2, "create");
		lua_pushcfunction(L, &FScriptDebugger::CoroutineWrapThunk);
		lua_setfield(L, -2, "wrap");
	}
	lua_pop(L, 1);
	UpdateHooks(L);
}

void FScriptDebugger::Detach(lua_State* L)
{
	if (MainState == nullptr || MainState != L)
	{
		return;
	}
	ReleaseHandles();
	lua_sethook(L, nullptr, 0, 0); // 상태를 곧 닫는다 — 코루틴 훅은 함께 사라진다
	lua_pushnil(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &GDebuggerKey);
	*static_cast<FScriptDebugger**>(lua_getextraspace(L)) = nullptr;
	MainState           = nullptr;
	RehitThread         = nullptr;
	StepThread          = nullptr;
	StepMode            = EStepMode::None;
	bPauseRequested     = false;
	bHooksInstalled     = false;
	HookMask            = 0;
	CachedFunction = nullptr; // 닫힌 상태의 함수를 가리키지 않게
	CachedEntry    = nullptr;
}

int FScriptDebugger::CoroutineCreateThunk(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_State* Thread = lua_newthread(L);
	lua_pushvalue(L, 1);
	lua_xmove(L, Thread, 1);
	// 기록 (훅을 나중에 걸거나 뗄 때 순회)
	lua_rawgetp(L, LUA_REGISTRYINDEX, &GThreadsKey);
	if (lua_istable(L, -1))
	{
		lua_pushvalue(L, -2);
		lua_pushboolean(L, 1);
		lua_rawset(L, -3);
	}
	lua_pop(L, 1);
	// lua_newthread는 만든 스레드(L)의 훅을 물려주지만, L이 훅을 걸기 전에 만든 스레드일 수 있으므로 지금 상태로 맞춘다
	if (FScriptDebugger* Debugger = FromState(L))
	{
		lua_sethook(Thread, Debugger->bHooksInstalled ? &FScriptDebugger::HookThunk : nullptr, Debugger->HookMask, 0);
	}
	return 1;
}

int FScriptDebugger::CoroutineWrapThunk(lua_State* L)
{
	CoroutineCreateThunk(L);
	lua_pushcclosure(L, &FScriptDebugger::WrapResumeThunk, 1);
	return 1;
}

// lcorolib.c auxwrap/auxresume와 같은 동작 (Lua 5.4)
int FScriptDebugger::WrapResumeThunk(lua_State* L)
{
	lua_State* Coroutine = lua_tothread(L, lua_upvalueindex(1));
	const int  ArgCount  = lua_gettop(L);
	int        Results   = -1;
	if (!lua_checkstack(Coroutine, ArgCount))
	{
		lua_pushliteral(L, "too many arguments to resume");
	}
	else
	{
		lua_xmove(L, Coroutine, ArgCount);
		int       ResultCount = 0;
		const int Status      = lua_resume(Coroutine, L, ArgCount, &ResultCount);
		if (Status == LUA_OK || Status == LUA_YIELD)
		{
			if (!lua_checkstack(L, ResultCount + 1))
			{
				lua_pop(Coroutine, ResultCount);
				lua_pushliteral(L, "too many results to resume");
			}
			else
			{
				lua_xmove(Coroutine, L, ResultCount);
				Results = ResultCount;
			}
		}
		else
		{
			lua_xmove(Coroutine, L, 1);
		}
	}
	if (Results >= 0)
	{
		return Results;
	}
	int Status = lua_status(Coroutine);
	if (Status != LUA_OK && Status != LUA_YIELD)
	{
		Status = lua_closethread(Coroutine, L);
		lua_xmove(Coroutine, L, 1);
	}
	if (Status != LUA_ERRMEM && lua_type(L, -1) == LUA_TSTRING)
	{
		luaL_where(L, 1);
		lua_insert(L, -2);
		lua_concat(L, 2);
	}
	return lua_error(L);
}

// ---------------------------------------------------------------- 훅

void FScriptDebugger::HookThunk(lua_State* L, lua_Debug* Ar)
{
	// 빠른 경로: 디버거 포인터는 스레드 추가 공간 (Attach가 주 스레드에 쓰고, 이후 만든 코루틴은 lua_newthread가 복사한다)
	if (FScriptDebugger* Debugger = *static_cast<FScriptDebugger**>(lua_getextraspace(L)))
	{
		Debugger->OnHook(L, Ar);
	}
}

void FScriptDebugger::OnHook(lua_State* L, lua_Debug* Ar)
{
	if (bPaused || !bEnabled || bSuspended)
	{
		return;
	}
	if (Ar->event == LUA_HOOKCALL || Ar->event == LUA_HOOKTAILCALL)
	{
		// 동적 모드: 들어가는 Lua 함수가 중단점 있는 파일이면 이 스레드의 줄 이벤트를 켜고, 아니면 끈다 (C 함수는 그대로)
		if (bDynamicLines && lua_getinfo(L, "f", Ar))
		{
			const bool bC = lua_iscfunction(L, -1) != 0;
			lua_pop(L, 1);
			if (!bC)
			{
				SetThreadLines(L, ResolveFunction(L, Ar) && CachedEntry != nullptr);
			}
		}
		return;
	}
	if (Ar->event == LUA_HOOKRET)
	{
		if (L == StepThread || L == RehitThread)
		{
			const int32 Depth = StackDepth(L); // 반환하는 함수 포함
			// 단계 시작 프레임이 반환 → 다음 파일 줄에서 어디서든 멈춘다 (호스트로 돌아감, 코루틴 끝)
			if ((StepMode == EStepMode::Over || StepMode == EStepMode::Out) && L == StepThread && Depth <= StepDepth)
			{
				bStepFrameReturned = true;
			}
			if (L == RehitThread && Depth <= RehitDepth)
			{
				RehitThread = nullptr; // 정지했던 프레임이 끝났다 — 같은 줄도 다시 멈출 수 있다
				UpdateHooks(L);
			}
		}
		// 동적 모드: 돌아갈 함수(레벨 1) 기준으로 줄 이벤트를 다시 맞춘다. 호스트(C++)로 돌아가면 끈다.
		// 돌아갈 곳이 C 함수면 그대로 둔다 (그 C 함수가 반환할 때 다시 맞춘다)
		if (bDynamicLines)
		{
			lua_Debug Caller;
			if (!lua_getstack(L, 1, &Caller))
			{
				SetThreadLines(L, false);
			}
			else if (lua_getinfo(L, "f", &Caller))
			{
				const bool bC = lua_iscfunction(L, -1) != 0;
				lua_pop(L, 1);
				if (!bC)
				{
					SetThreadLines(L, ResolveFunction(L, &Caller) && CachedEntry != nullptr);
				}
			}
		}
		return;
	}
	if (Ar->event != LUA_HOOKLINE)
	{
		return;
	}
	++LineHookCount;

	// 정지했던 줄 다시 멈춤 막기: 한 문장의 명령이 여러 줄에 걸치면(여러 줄 클로저 인자 등) Lua는 같은 줄 이벤트를 다시 낸다.
	// 같은 프레임에서 뒤로 점프(반복문)하지 않고 다른 줄을 거쳐 같은 줄로 돌아온 것은 같은 문장이므로 중단점을 건너뛴다
	bool bSuppressBreakpoint = false;
	if (L == RehitThread)
	{
		const int32 Depth = StackDepth(L);
		if (Depth < RehitDepth)
		{
			RehitThread = nullptr;
		}
		else if (Depth == RehitDepth)
		{
			const int32 Line = Ar->currentline;
			if (Line < RehitLine || (Line == RehitLine && RehitLastLine == RehitLine))
			{
				RehitThread = nullptr; // 반복문으로 돌아옴
			}
			else if (Line == RehitLine)
			{
				bSuppressBreakpoint = true;
			}
			RehitLastLine = Line;
		}
	}

	// 함수별 캐시 (ResolveFunction): 같은 함수가 계속 도는 동안 청크 이름을 다시 읽지 않는다
	if (!ResolveFunction(L, Ar))
	{
		return; // 파일이 아닌 청크 (엔진 도우미, RunString)
	}

	if (bPauseRequested)
	{
		Pause(L, L, EScriptPauseReason::PauseRequest, std::string(), 0);
		return;
	}
	if (StepMode != EStepMode::None)
	{
		bool bStop = StepMode == EStepMode::Into || bStepFrameReturned;
		if (!bStop && L == StepThread)
		{
			const int32 Depth = StackDepth(L);
			bStop             = StepMode == EStepMode::Over ? Depth <= StepDepth : Depth < StepDepth;
		}
		if (bStop)
		{
			Pause(L, L, EScriptPauseReason::Step, std::string(), 0);
			return;
		}
	}

	if (CachedEntry == nullptr || bSuppressBreakpoint)
	{
		return;
	}
	for (const int32 Index : CachedEntry->Indices)
	{
		FScriptBreakpoint& Breakpoint = Breakpoints[static_cast<size_t>(Index)];
		if (Breakpoint.Line != Ar->currentline)
		{
			continue;
		}
		std::string Message;
		if (!Breakpoint.Condition.empty())
		{
			std::string Error;
			const bool  bTrue = EvaluateCondition(L, Breakpoint.Condition, Error);
			if (!Error.empty())
			{
				Message = "조건식 오류: " + Error;
			}
			else if (!bTrue)
			{
				return;
			}
		}
		++Breakpoint.HitCount;
		Pause(L, L, EScriptPauseReason::Breakpoint, std::move(Message), 0);
		return;
	}
}

void FScriptDebugger::OnUnhandledError(lua_State* L, const char* Message, int32 FrameOffset)
{
	if (!bBreakOnError || !bEnabled || bSuspended || bPaused || MainState == nullptr || !PauseHandler || !lua_checkstack(L, 40))
	{
		return;
	}
	Pause(L, L, EScriptPauseReason::Error, Message != nullptr ? Message : "(오류 값이 문자열이 아님)", FrameOffset);
}

void FScriptDebugger::OnCoroutineError(lua_State* Thread, lua_State* Work, const char* Message)
{
	if (!bBreakOnError || !bEnabled || bSuspended || bPaused || MainState == nullptr || !PauseHandler || !lua_checkstack(Work, 40))
	{
		return;
	}
	Pause(Thread, Work, EScriptPauseReason::Error, Message != nullptr ? Message : "(오류 값이 문자열이 아님)", 0);
}

void FScriptDebugger::Pause(lua_State* Thread, lua_State* Work, EScriptPauseReason Reason, std::string Message, int32 FrameOffset)
{
	if (bPaused || !PauseHandler || !lua_checkstack(Work, 20) || !lua_checkstack(Thread, 4))
	{
		StepMode        = EStepMode::None;
		bPauseRequested = false;
		return;
	}

	// 호출 스택
	PauseState         = FScriptPauseState();
	PauseState.Reason  = Reason;
	PauseState.Message = std::move(Message);
	PauseState.bInCoroutine = Thread != MainState;
	PauseState.CurrentFrame = -1;
	FrameLevels.clear();
	lua_Debug Ar;
	for (int32 Level = FrameOffset; lua_getstack(Thread, Level, &Ar) && static_cast<int32>(FrameLevels.size()) < MaxStackFrames; ++Level)
	{
		if (!lua_getinfo(Thread, "nSl", &Ar))
		{
			continue;
		}
		FScriptStackFrame Frame;
		Frame.Line  = Ar.currentline;
		Frame.bFile = Ar.source != nullptr && Ar.source[0] == '@' && Ar.what != nullptr && std::strcmp(Ar.what, "C") != 0;
		Frame.File  = Frame.bFile ? std::string(Ar.source + 1) : std::string(Ar.short_src);
		if (Ar.name != nullptr)
		{
			Frame.Function = Ar.namewhat != nullptr && std::strcmp(Ar.namewhat, "method") == 0 ? std::string(":") + Ar.name : std::string(Ar.name);
		}
		else if (Ar.what != nullptr && std::strcmp(Ar.what, "main") == 0)
		{
			Frame.Function = "(파일 청크)";
		}
		else if (Frame.bFile)
		{
			// 엔진(C++)이 부른 메서드는 이름 정보가 없다: self의 클래스(메타테이블 __index)에서 같은 함수를 찾는다
			std::string Found;
			if (lua_getinfo(Thread, "f", &Ar))
			{
				lua_xmove(Thread, Work, 1);
				const int   Function = lua_gettop(Work);
				const char* Name     = lua_getlocal(Thread, &Ar, 1);
				if (Name != nullptr)
				{
					lua_xmove(Thread, Work, 1);
					if (std::strcmp(Name, "self") == 0 && lua_istable(Work, -1) && lua_getmetatable(Work, -1))
					{
						lua_pushliteral(Work, "__index");
						lua_rawget(Work, -2);
						if (lua_istable(Work, -1))
						{
							lua_pushnil(Work);
							while (Found.empty() && lua_next(Work, -2) != 0)
							{
								if (lua_type(Work, -2) == LUA_TSTRING && lua_rawequal(Work, -1, Function))
								{
									Found = std::string("self:") + lua_tostring(Work, -2);
									lua_pop(Work, 2);
									break;
								}
								lua_pop(Work, 1);
							}
						}
					}
				}
				lua_settop(Work, Function - 1);
			}
			Frame.Function = !Found.empty() ? Found : std::format("function <{}:{}>", Frame.File, Ar.linedefined);
		}
		else
		{
			Frame.Function = "[C]";
		}
		if (PauseState.CurrentFrame < 0 && Frame.bFile)
		{
			PauseState.CurrentFrame = static_cast<int32>(PauseState.Stack.size());
			PauseState.File         = Frame.File;
			PauseState.Line         = Frame.Line;
		}
		PauseState.Stack.push_back(std::move(Frame));
		FrameLevels.push_back(Level);
	}
	if (PauseState.CurrentFrame < 0)
	{
		PauseState.CurrentFrame = 0;
	}

	const int Top    = lua_gettop(Work);
	bPaused          = true;
	PausedThread     = Thread;
	WorkThread       = Work;
	ResumeCommand    = EResume::None;
	StepMode         = EStepMode::None;
	StepThread       = nullptr;
	bPauseRequested  = false;
	bStepFrameReturned = false;
	UpdateHooks(Work); // 정지 중에는 훅 없음 (조사식 평가가 다시 걸리지 않게)
	E_LOG(LogScript, Display, "스크립트 디버거 정지 ({}): {}:{}{}", ToString(Reason), PauseState.File, PauseState.Line,
	      PauseState.Message.empty() ? std::string() : " — " + PauseState.Message.substr(0, PauseState.Message.find('\n')));

	PauseHandler(*this);

	lua_settop(Work, Top);
	ReleaseHandles();
	bPaused      = false;
	PausedThread = nullptr;
	WorkThread   = nullptr;
	if (MainState == nullptr)
	{
		return; // 정지 중 분리됨
	}
	if (Reason != EScriptPauseReason::Error && !bSuspended && bEnabled)
	{
		switch (ResumeCommand)
		{
		case EResume::Into: StepMode = EStepMode::Into; break;
		case EResume::Over: StepMode = EStepMode::Over; break;
		case EResume::Out:  StepMode = EStepMode::Out; break;
		default:            StepMode = EStepMode::None; break;
		}
		if (StepMode != EStepMode::None)
		{
			StepThread = Thread;
			StepDepth  = StackDepth(Thread) - FrameOffset;
		}
	}
	// 줄 훅에서 멈췄으면 그 프레임의 같은 줄(같은 문장)에서 다시 멈추지 않게 (OnHook 참고)
	RehitThread = nullptr;
	if (FrameOffset == 0 && Reason != EScriptPauseReason::Error && !PauseState.Stack.empty() && !bSuspended)
	{
		RehitThread   = Thread;
		RehitDepth    = StackDepth(Thread);
		RehitLine     = PauseState.Stack.front().Line;
		RehitLastLine = RehitLine;
	}
	UpdateHooks(Work);
}

// ---------------------------------------------------------------- 조회

void FScriptDebugger::ReleaseHandles()
{
	if (MainState != nullptr)
	{
		for (const FHandle& Handle : Handles)
		{
			luaL_unref(MainState, LUA_REGISTRYINDEX, Handle.Ref);
		}
	}
	Handles.clear();
}

bool FScriptDebugger::GetFrame(int32 FrameIndex, lua_Debug& OutAr)
{
	if (!bPaused || FrameIndex < 0 || FrameIndex >= static_cast<int32>(FrameLevels.size()) || !lua_checkstack(WorkThread, 20) ||
	    !lua_checkstack(PausedThread, 4))
	{
		return false;
	}
	return lua_getstack(PausedThread, FrameLevels[static_cast<size_t>(FrameIndex)], &OutAr) != 0;
}

FScriptVariable FScriptDebugger::MakeVariable(std::string Name, int32 Depth)
{
	lua_State* W = WorkThread != nullptr ? WorkThread : MainState;
	FScriptVariable Variable;
	Variable.Name = std::move(Name);
	const int Type = lua_type(W, -1);
	Variable.Type  = lua_typename(W, Type);
	switch (Type)
	{
	case LUA_TSTRING:
	{
		size_t      Length = 0;
		const char* Text   = lua_tolstring(W, -1, &Length);
		Variable.Value     = QuoteString(Text, Length);
		break;
	}
	case LUA_TTABLE:
	{
		int32 Count = 0;
		lua_pushnil(W);
		while (lua_next(W, -2) != 0)
		{
			lua_pop(W, 1);
			if (++Count >= MaxCountedEntries)
			{
				lua_pop(W, 1);
				break;
			}
		}
		const lua_Unsigned Length = lua_rawlen(W, -1);
		Variable.Value = Count >= MaxCountedEntries ? std::format("{{…}} 항목 {}+", Count)
		                 : Length > 0                ? std::format("{{…}} 항목 {} (#{})", Count, Length)
		                                             : std::format("{{…}} 항목 {}", Count);
		const bool bHasMetatable = lua_getmetatable(W, -1) != 0;
		if (bHasMetatable)
		{
			lua_pop(W, 1);
			Variable.Value += " +메타테이블";
		}
		if ((Count > 0 || bHasMetatable) && Depth < MaxExpandDepth && Handles.size() < MaxHandles)
		{
			lua_pushvalue(W, -1);
			Handles.push_back({ luaL_ref(W, LUA_REGISTRYINDEX), Depth });
			Variable.ChildHandle = static_cast<uint32>(Handles.size());
		}
		break;
	}
	case LUA_TFUNCTION:
		Variable.Value = DescribeFunction(W);
		break;
	case LUA_TUSERDATA:
	case LUA_TLIGHTUSERDATA:
		Variable.Value = ProtectedToString(W, -1);
		break;
	case LUA_TTHREAD:
		Variable.Value = "thread";
		break;
	default:
		Variable.Value = ScalarToString(W, -1);
		break;
	}
	return Variable;
}

std::vector<FScriptVariable> FScriptDebugger::GetLocals(int32 FrameIndex)
{
	std::vector<FScriptVariable> Result;
	lua_Debug                    Ar;
	if (!GetFrame(FrameIndex, Ar))
	{
		return Result;
	}
	for (int Index = 1;; ++Index)
	{
		const char* Name = lua_getlocal(PausedThread, &Ar, Index);
		if (Name == nullptr)
		{
			break;
		}
		lua_xmove(PausedThread, WorkThread, 1);
		if (Name[0] != '(') // (temporary), (for state) 등 내부 슬롯
		{
			Result.push_back(MakeVariable(Name, 0));
		}
		lua_pop(WorkThread, 1);
	}
	return Result;
}

std::vector<FScriptVariable> FScriptDebugger::GetUpvalues(int32 FrameIndex)
{
	std::vector<FScriptVariable> Result;
	lua_Debug                    Ar;
	if (!GetFrame(FrameIndex, Ar) || !lua_getinfo(PausedThread, "f", &Ar))
	{
		return Result;
	}
	lua_xmove(PausedThread, WorkThread, 1);
	const int Function = lua_gettop(WorkThread);
	for (int Index = 1;; ++Index)
	{
		const char* Name = lua_getupvalue(WorkThread, Function, Index);
		if (Name == nullptr)
		{
			break;
		}
		if (std::strcmp(Name, "_ENV") != 0) // 전역 표 (너무 크다 — 조사식으로 본다)
		{
			Result.push_back(MakeVariable(Name[0] != '\0' ? Name : std::format("?{}", Index), 0));
		}
		lua_pop(WorkThread, 1);
	}
	lua_settop(WorkThread, Function - 1);
	return Result;
}

std::vector<FScriptVariable> FScriptDebugger::GetChildren(uint32 Handle)
{
	std::vector<FScriptVariable> Result;
	if (!bPaused || Handle == 0 || Handle > Handles.size() || !lua_checkstack(WorkThread, 20))
	{
		return Result;
	}
	lua_State*    W      = WorkThread;
	const FHandle Parent = Handles[Handle - 1];
	lua_rawgeti(W, LUA_REGISTRYINDEX, Parent.Ref);
	const int Table = lua_gettop(W);

	struct FEntry
	{
		FScriptVariable Variable;
		int32           Kind = 2; // 0 정수 키, 1 문자열 키, 2 기타
		lua_Integer     Integer = 0;
		std::string     String;
	};
	std::vector<FEntry> Entries;
	int32               Skipped = 0;
	lua_pushnil(W);
	while (lua_next(W, Table) != 0)
	{
		if (static_cast<int32>(Entries.size()) >= MaxChildren)
		{
			++Skipped;
			lua_pop(W, 1);
			continue;
		}
		FEntry Entry;
		const std::string Name = KeyToName(W, -2, Entry.Kind, Entry.Integer, Entry.String);
		Entry.Variable         = MakeVariable(Name, Parent.Depth + 1);
		Entries.push_back(std::move(Entry));
		lua_pop(W, 1);
	}
	std::stable_sort(Entries.begin(), Entries.end(), [](const FEntry& A, const FEntry& B) {
		if (A.Kind != B.Kind)
		{
			return A.Kind < B.Kind;
		}
		return A.Kind == 0 ? A.Integer < B.Integer : A.Kind == 1 ? A.String < B.String : false;
	});
	Result.reserve(Entries.size() + 2);
	for (FEntry& Entry : Entries)
	{
		Result.push_back(std::move(Entry.Variable));
	}
	if (Skipped > 0)
	{
		Result.push_back({ "…", std::format("항목 {}개 더 (표시 상한 {})", Skipped, MaxChildren), "", 0 });
	}
	if (lua_getmetatable(W, Table))
	{
		Result.push_back(MakeVariable("(메타테이블)", Parent.Depth + 1));
		lua_pop(W, 1);
	}
	lua_settop(W, Table - 1);
	return Result;
}

void FScriptDebugger::PushFrameEnvironment(lua_State* Thread, lua_State* Work, lua_Debug& Ar)
{
	lua_newtable(Work);
	const int Env = lua_gettop(Work);
	lua_rawgeti(Work, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); // 기본 __index (함수에 _ENV upvalue가 있으면 그것)
	const int Fallback = lua_gettop(Work);
	if (lua_getinfo(Thread, "f", &Ar))
	{
		lua_xmove(Thread, Work, 1);
		const int Function = lua_gettop(Work);
		for (int Index = 1;; ++Index)
		{
			const char* Name = lua_getupvalue(Work, Function, Index);
			if (Name == nullptr)
			{
				break;
			}
			if (std::strcmp(Name, "_ENV") == 0)
			{
				lua_replace(Work, Fallback);
			}
			else if (Name[0] != '\0')
			{
				lua_setfield(Work, Env, Name);
			}
			else
			{
				lua_pop(Work, 1);
			}
		}
		lua_pop(Work, 1); // 함수
	}
	// 지역 변수 (뒤에 선언된 같은 이름이 앞을 가린다 — Lua 스코프와 같게 번호 순서대로 덮어쓴다)
	for (int Index = 1;; ++Index)
	{
		const char* Name = lua_getlocal(Thread, &Ar, Index);
		if (Name == nullptr)
		{
			break;
		}
		lua_xmove(Thread, Work, 1);
		if (Name[0] != '(')
		{
			lua_setfield(Work, Env, Name);
		}
		else
		{
			lua_pop(Work, 1);
		}
	}
	lua_newtable(Work);
	lua_pushvalue(Work, Fallback);
	lua_setfield(Work, -2, "__index");
	lua_setmetatable(Work, Env);
	lua_settop(Work, Env);
}

namespace
{
	// 식(안 되면 문장)을 컴파일해 Env를 _ENV로 쓰는 함수로 올린다. 실패하면 false + 오류 메시지를 올린다
	bool LoadWithEnvironment(lua_State* W, const std::string& Code, int Env)
	{
		const std::string Expression = "return " + Code;
		if (luaL_loadbufferx(W, Expression.data(), Expression.size(), "=조사식", "t") != LUA_OK)
		{
			lua_pop(W, 1);
			if (luaL_loadbufferx(W, Code.data(), Code.size(), "=조사식", "t") != LUA_OK)
			{
				return false;
			}
		}
		lua_pushvalue(W, Env);
		if (lua_setupvalue(W, -2, 1) == nullptr)
		{
			lua_pop(W, 1);
		}
		return true;
	}

	std::string ErrorText(lua_State* W)
	{
		const char* Text = lua_tostring(W, -1);
		return Text != nullptr ? Text : "(오류 값이 문자열이 아님)";
	}
} // namespace

bool FScriptDebugger::EvaluateCondition(lua_State* L, const std::string& Condition, std::string& OutError)
{
	lua_Debug Ar;
	if (!lua_checkstack(L, 20) || !lua_getstack(L, 0, &Ar))
	{
		OutError = "스택을 읽을 수 없습니다";
		return false;
	}
	const int Top = lua_gettop(L);
	PushFrameEnvironment(L, L, Ar);
	const int Env = lua_gettop(L);
	bool      bResult = false;
	if (!LoadWithEnvironment(L, Condition, Env))
	{
		OutError = ErrorText(L);
	}
	else if (lua_pcall(L, 0, 1, 0) != LUA_OK)
	{
		OutError = ErrorText(L);
	}
	else
	{
		bResult = lua_toboolean(L, -1) != 0;
	}
	lua_settop(L, Top);
	return bResult;
}

bool FScriptDebugger::Evaluate(int32 FrameIndex, const std::string& Expression, FScriptVariable& OutResult)
{
	OutResult      = FScriptVariable();
	OutResult.Name = Expression;
	lua_Debug Ar;
	if (!GetFrame(FrameIndex, Ar))
	{
		OutResult.Value = "정지 중이 아닙니다";
		return false;
	}
	lua_State* W   = WorkThread;
	const int  Top = lua_gettop(W);
	PushFrameEnvironment(PausedThread, W, Ar);
	const int Env = lua_gettop(W);
	bool      bOk = LoadWithEnvironment(W, Expression, Env) && lua_pcall(W, 0, 1, 0) == LUA_OK;
	if (bOk)
	{
		OutResult = MakeVariable(Expression, 0);
	}
	else
	{
		OutResult.Value = ErrorText(W);
	}
	lua_settop(W, Top);
	return bOk;
}

#pragma once

#include "Core/CoreTypes.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State; // Lua 타입 이름만 (헤더는 ScriptDebugger.cpp에서만 포함)
struct lua_Debug;

// Lua 스크립트 디버거 코어 (Phase 47 사이드). 앱(에디터)이 하나 만들어 FScriptSystem::SetDebugger로 연결한다 — 연결하지 않으면
// (런타임/서버/테스트 기본) 디버거가 없고 훅·오류 처리기도 원래대로다.
//
// 규칙
// - 훅: lua_sethook(줄 이벤트, 단계 실행 중 Out/Over는 반환 이벤트도)은 "켜짐 + 연결됨 + 정지 중 아님 + (켜진 중단점 또는 단계 실행
//   또는 일시 정지 요청)"일 때만 모든 스레드(주 스레드 + 이 상태에서 만든 코루틴 — coroutine.create/wrap을 감싸 약한 표에 기록)에
//   건다. 조건이 사라지면 바로 뗀다 → 중단점이 없으면 실행 비용 0 (오류 처리기 교체만 남는데 오류가 날 때만 불린다).
//   중단점만 있을 때는 "동적" 모드: 호출/반환 이벤트만 받고, 지금 함수가 중단점 있는 파일일 때만 그 스레드에 줄 이벤트를 켠다
//   (다른 스크립트의 반복문은 줄 훅 비용 없음). 오류로 풀린 스택(pcall)은 반환 이벤트가 없어 다음 호출/반환까지 판단이 늦을 수 있다.
//   단계 실행·일시 정지 요청 중에는 모든 줄 이벤트를 받는다.
// - 같은 문장이 여러 줄에 걸치면 Lua가 같은 줄 이벤트를 다시 내므로, 멈췄던 프레임에서는 뒤로 점프(반복문) 없이 같은 줄로 돌아온
//   이벤트를 무시한다 (그 프레임이 반환하면 해제).
// - 중단점 = (파일, 줄). 파일은 청크 이름("@" + Content 기준 경로)과 대소문자·구분자 무시로 비교. 조건식은 그 줄에서 정지 프레임
//   환경(지역 변수 > upvalue > 전역)으로 평가해 참일 때만 멈추고, 평가 오류면 멈추고 오류를 보여 준다.
// - 단계 실행: Into = 다음 파일 줄(어느 함수·스레드든). Over = 같은 스레드에서 깊이 <= 시작 깊이인 줄. Out = 같은 스레드에서 깊이 < 시작
//   깊이인 줄. Over/Out은 시작 프레임이 반환하면(호스트 C++로 돌아감/코루틴 끝) 다음 파일 줄에서 어디서든 멈춘다. 코루틴 안에서 시작한
//   Over/Out은 그 코루틴이 yield하면 다른 스레드 줄은 건너뛰고 그 코루틴이 다시 재개될 때 이어간다(Wait 뒤 다음 줄). 파일이 아닌 청크
//   ("=Wait", "=RunString" 등 엔진 도우미)에서는 멈추지 않는다. 오류 정지에서의 단계 명령은 계속과 같다.
// - 오류 정지(bBreakOnError): 처리되지 않은 스크립트 오류(엔진이 부른 메서드·콜백 — 메시지 처리기에서, Coroutine.Start 코루틴은 재개
//   실패 직후 그 코루틴 스택으로)에서 멈춘다. 스크립트가 직접 pcall로 잡은 오류는 멈추지 않는다.
// - 정지: 정지 처리기(SetPauseHandler)를 부르고 돌아올 때까지 Lua는 그 자리에 멈춰 있다. 처리기는 Continue/Step*을 부른 뒤 돌아오고,
//   명령 없이 돌아오면 계속. 정지 중에는 훅을 떼므로 조사식 평가가 다시 중단점에 걸리지 않는다. 정지 중 조회(GetLocals 등)는 처리기
//   안에서만 유효하고, 테이블 펼치기 핸들은 재개하면 모두 무효가 된다.
// - 조사식 평가는 정지 프레임의 지역 변수/upvalue 사본을 담은 환경에서 "return <식>"(안 되면 문장)으로 실행한다. 지역 변수 대입은
//   원래 변수에 반영되지 않지만 함수 호출·테이블 수정은 실제로 일어난다(부작용 주의). 값이 nil인 지역 변수는 같은 이름 전역이 보인다.
// - 스레드: 메인 스레드 전용 (Lua와 같은 스레드).
enum class EScriptPauseReason : uint8
{
	Breakpoint,
	Step,
	PauseRequest,
	Error,
};

const char* ToString(EScriptPauseReason Reason);

struct FScriptBreakpoint
{
	std::string File;      // Content 기준 경로 ("Scripts/Player.lua")
	int32       Line = 0;  // 1부터
	std::string Condition; // 비었으면 항상 멈춤
	bool        bEnabled = true;
	uint32      HitCount = 0; // 이번 프로세스에서 멈춘 횟수 (저장하지 않음)
};

struct FScriptStackFrame
{
	std::string Function;      // "OnUpdate", "Helper", "(파일 청크)", "[C] print" ...
	std::string File;          // Content 기준 경로 (파일 청크만, 아니면 청크 이름 그대로)
	int32       Line  = -1;    // 현재 줄 (C 함수 -1)
	bool        bFile = false; // 파일 청크의 Lua 함수 (소스를 보여 줄 수 있음)
};

struct FScriptVariable
{
	std::string Name;
	std::string Value;           // 표시 문자열 (길면 자름)
	std::string Type;            // Lua 타입 이름
	uint32      ChildHandle = 0; // 0 = 펼칠 수 없음. GetChildren으로 지연 조회 (재개하면 무효)
};

struct FScriptPauseState
{
	EScriptPauseReason             Reason = EScriptPauseReason::Breakpoint;
	std::string                    File;    // 멈춘 줄의 파일 (Content 기준)
	int32                          Line = 0;
	std::string                    Message; // 오류 메시지(오류 정지), 조건식 오류(중단점)
	bool                           bInCoroutine = false;
	std::vector<FScriptStackFrame> Stack;   // 0 = 가장 안쪽
	int32                          CurrentFrame = 0; // 처음 선택할 프레임 (가장 안쪽 파일 프레임)
};

class FScriptDebugger
{
public:
	using FPauseHandler = std::function<void(FScriptDebugger&)>;

	FScriptDebugger();
	~FScriptDebugger();
	FScriptDebugger(const FScriptDebugger&)            = delete;
	FScriptDebugger& operator=(const FScriptDebugger&) = delete;

	// 꺼지면 중단점·오류 정지·일시 정지 요청을 모두 무시 (훅 해제)
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }
	// 이번 플레이 동안만 무시 (정지 중 플레이 정지/창 닫기: 재개 후 남은 코드가 다시 멈추지 않게). 다음 Attach(플레이 시작)에서 풀린다
	void SuspendUntilNextSession();
	void SetBreakOnError(bool bInBreakOnError) { bBreakOnError = bInBreakOnError; }
	bool GetBreakOnError() const { return bBreakOnError; }
	void SetPauseHandler(FPauseHandler Handler) { PauseHandler = std::move(Handler); }

	// ---- 중단점 (언제든 — 정지 중에 바꾸면 재개할 때 반영)
	const std::vector<FScriptBreakpoint>& GetBreakpoints() const { return Breakpoints; }
	void SetBreakpoints(std::vector<FScriptBreakpoint> InBreakpoints);
	bool ToggleBreakpoint(const std::string& File, int32 Line); // 반환: 이제 있는가
	void AddBreakpoint(const FScriptBreakpoint& Breakpoint);    // 같은 (파일, 줄)이면 교체
	bool RemoveBreakpoint(const std::string& File, int32 Line);
	void SetBreakpointEnabled(const std::string& File, int32 Line, bool bInEnabled);
	void SetBreakpointCondition(const std::string& File, int32 Line, const std::string& Condition);
	const FScriptBreakpoint* FindBreakpoint(const std::string& File, int32 Line) const;
	void ClearBreakpoints();
	// 파일이 바뀌었을 때(핫 리로드): 줄 수를 넘는 중단점 제거. 반환: 제거한 수
	int32 PruneBreakpoints(const std::string& File, int32 LineCount);
	// 중단점 목록이 바뀔 때마다 증가 (저장 판단, 적중 수 변화는 제외)
	uint32 GetBreakpointRevision() const { return BreakpointRevision; }

	// 파일 키 정규화 (소문자, '/' 구분, 앞 "./" 제거)
	static std::string NormalizeFile(std::string_view File);

	// ---- 실행 제어
	void RequestPause(); // 다음 파일 줄에서 멈춤
	bool IsPaused() const { return bPaused; }
	bool IsAttached() const { return MainState != nullptr; }
	const FScriptPauseState& GetPauseState() const { return PauseState; }
	// 정지 처리기 안에서 재개 명령이 내려졌는가 (앱의 중첩 루프 종료 조건)
	bool IsResumeRequested() const { return ResumeCommand != EResume::None; }
	void Continue();
	void StepOver();
	void StepInto();
	void StepOut();

	// ---- 정지 중 조회 (정지 처리기 안에서만, 아니면 빈 결과)
	std::vector<FScriptVariable> GetLocals(int32 FrameIndex);
	std::vector<FScriptVariable> GetUpvalues(int32 FrameIndex);
	std::vector<FScriptVariable> GetChildren(uint32 Handle);
	// 식 평가 (조사식). 실패하면 false + OutResult.Value = 오류 메시지
	bool Evaluate(int32 FrameIndex, const std::string& Expression, FScriptVariable& OutResult);

	// ---- 통계 (성능 측정/테스트)
	uint64 GetLineHookCount() const { return LineHookCount; }
	bool   AreHooksInstalled() const { return bHooksInstalled; }

	// ---- FLuaRuntime 연결 (Scripting 모듈 내부): Attach는 바인딩 등록 전 (coroutine.create/wrap 교체)
	void Attach(lua_State* L);
	void Detach(lua_State* L);
	// 메시지 처리기(오류 직후, 스택 보존)에서: 오류 정지 대상이면 멈춘다. FrameOffset = 처리기 자신 등 건너뛸 프레임 수
	void OnUnhandledError(lua_State* L, const char* Message, int32 FrameOffset);
	// Coroutine.Start 코루틴 재개 실패 직후 (Thread = 오류로 끝난 코루틴, Work = 호출 중인 스레드)
	void OnCoroutineError(lua_State* Thread, lua_State* Work, const char* Message);
	// lua_State에 연결된 디버거 (없으면 nullptr)
	static FScriptDebugger* FromState(lua_State* L);

private:
	enum class EStepMode : uint8
	{
		None,
		Into,
		Over,
		Out,
	};
	enum class EResume : uint8
	{
		None,
		Continue,
		Into,
		Over,
		Out,
	};
	struct FFileBreakpoints
	{
		std::vector<int32> Indices; // Breakpoints 안 번호
	};
	struct FHandle
	{
		int   Ref   = 0;
		int32 Depth = 0;
	};

	static void HookThunk(lua_State* L, lua_Debug* Ar);
	// coroutine.create/wrap 대체 (원본과 같은 동작 + 새 스레드를 약한 표에 기록하고 지금 훅을 건다)
	static int  CoroutineCreateThunk(lua_State* L);
	static int  CoroutineWrapThunk(lua_State* L);
	static int  WrapResumeThunk(lua_State* L);
	void        OnHook(lua_State* L, lua_Debug* Ar);
	void        Pause(lua_State* Thread, lua_State* Work, EScriptPauseReason Reason, std::string Message, int32 FrameOffset);
	void        RebuildIndex();
	// 훅 설치/해제/모드 변경 (bForce: 중단점이 바뀌어 스레드별 줄 이벤트를 다시 판단)
	void        UpdateHooks(lua_State* Work, bool bForce = false);
	void        ApplyThreadHook(lua_State* Thread);
	void        SetThreadLines(lua_State* Thread, bool bLines); // 동적 모드: 이 스레드 줄 이벤트 켜기/끄기
	// Ar 프레임 함수의 캐시 항목 (bCachedFile/CachedEntry 갱신). 반환: 파일 청크의 Lua 함수인가
	bool        ResolveFunction(lua_State* L, lua_Debug* Ar);
	bool        WantsHooks() const;
	bool        EvaluateCondition(lua_State* L, const std::string& Condition, std::string& OutError);
	void        ReleaseHandles();
	// 정지 프레임 → 그 스레드의 lua_Debug (실패 시 false)
	bool        GetFrame(int32 FrameIndex, lua_Debug& OutAr);
	// Work 스택 맨 위 값을 표시 형식으로 (펼칠 수 있으면 핸들 발급). 값은 그대로 둔다
	FScriptVariable MakeVariable(std::string Name, int32 Depth);
	// 정지 프레임 환경 표를 Work 스택에 올린다
	void        PushFrameEnvironment(lua_State* Thread, lua_State* Work, lua_Debug& Ar);

	bool          bEnabled      = true;
	bool          bSuspended    = false;
	bool          bBreakOnError = true;
	FPauseHandler PauseHandler;

	std::vector<FScriptBreakpoint>                    Breakpoints;
	std::unordered_map<std::string, FFileBreakpoints> BreakpointIndex; // 정규화 파일 → 켜진 중단점
	uint32                                            BreakpointRevision = 0;
	// 줄 이벤트마다 청크 이름을 읽지 않도록 마지막 함수(클로저) 결과를 기억 (함수는 레지스트리에 잡아 둔다)
	const void*             CachedFunction = nullptr;
	bool                    bCachedFile    = false;
	const FFileBreakpoints* CachedEntry    = nullptr;
	// 줄 훅에서 멈춘 프레임: 같은 문장이 같은 줄 이벤트를 다시 내도 중단점을 건너뛴다 (OnHook)
	lua_State* RehitThread   = nullptr;
	int32      RehitDepth    = 0;
	int32      RehitLine     = 0;
	int32      RehitLastLine = 0;

	lua_State* MainState      = nullptr;
	bool       bHooksInstalled = false;
	int        HookMask        = 0;     // 모든 스레드 공통 (동적 모드면 줄 이벤트는 스레드별로 더한다)
	bool       bDynamicLines   = false; // 중단점만 있음: 호출/반환 이벤트로 중단점 파일 함수 안에서만 줄 이벤트
	bool       bPauseRequested = false;
	EStepMode  StepMode        = EStepMode::None;
	lua_State* StepThread      = nullptr;
	int32      StepDepth       = 0;
	bool       bStepFrameReturned = false;

	bool              bPaused      = false;
	EResume           ResumeCommand = EResume::None;
	FScriptPauseState PauseState;
	lua_State*        PausedThread = nullptr; // 프레임을 읽는 스레드
	lua_State*        WorkThread   = nullptr; // 값을 올리고 함수를 부르는 스레드
	std::vector<int32> FrameLevels;           // PauseState.Stack 번호 → Lua 레벨
	std::vector<FHandle> Handles;             // 핸들 - 1 = 번호

	uint64 LineHookCount = 0;
};

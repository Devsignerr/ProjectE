#include "Core/Log.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptDebugger.h"
#include "Scripting/ScriptSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

E_DECLARE_LOG_CATEGORY(LogScriptDebuggerTest)
E_DEFINE_LOG_CATEGORY(LogScriptDebuggerTest, Log)

// Lua 스크립트 디버거 코어 (Phase 47 사이드, 규칙은 Scripting/ScriptDebugger.h 머리 주석).
// 에디터의 중첩 루프 대신 테스트 정지 처리기가 정지 지점을 기록하고 바로 다음 명령을 내린다.
namespace
{
	std::filesystem::path GetDebuggerTestContentDirectory()
	{
		static const std::filesystem::path Directory = [] {
			std::filesystem::path Path = FTestRegistry::GetTempDirectory() / L"ProjectEScriptDebuggerTests";
			std::error_code       ErrorCode;
			std::filesystem::remove_all(Path, ErrorCode);
			std::filesystem::create_directories(Path / L"Scripts");
			return Path;
		}();
		return Directory;
	}

	void WriteDebugScript(const char* RelativePath, const char* Source)
	{
		std::ofstream File(GetDebuggerTestContentDirectory() / RelativePath, std::ios::binary | std::ios::trunc);
		File << Source;
	}

	FEntity AddDebugScriptedEntity(FScene& Scene, const char* Name, const char* ScriptAsset)
	{
		const FEntity     Entity    = Scene.CreateEntity(Name);
		FScriptComponent& Component = Scene.GetRegistry().Emplace<FScriptComponent>(Entity);
		Component.ScriptAsset       = ScriptAsset;
		return Entity;
	}

	struct FPauseRecord
	{
		EScriptPauseReason Reason = EScriptPauseReason::Breakpoint;
		std::string        File;
		int32              Line = 0;
		std::string        Message;
		bool               bInCoroutine = false;
		size_t             Depth = 0;
		std::string        TopFunction;
	};

	// 정지마다 기록 + 명령 목록 순서대로 (없으면 계속). Inspect가 있으면 정지 중에 부른다
	struct FDebugHarness
	{
		FScriptDebugger                                  Debugger;
		std::vector<FPauseRecord>                        Pauses;
		std::vector<std::function<void(FScriptDebugger&)>> Commands;
		std::function<void(FScriptDebugger&)>            Inspect;

		FDebugHarness()
		{
			Debugger.SetPauseHandler([this](FScriptDebugger& Self) {
				const FScriptPauseState& State = Self.GetPauseState();
				FPauseRecord             Record;
				Record.Reason       = State.Reason;
				Record.File         = State.File;
				Record.Line         = State.Line;
				Record.Message      = State.Message;
				Record.bInCoroutine = State.bInCoroutine;
				Record.Depth        = State.Stack.size();
				Record.TopFunction  = State.Stack.empty() ? std::string() : State.Stack[static_cast<size_t>(State.CurrentFrame)].Function;
				Pauses.push_back(Record);
				if (Inspect)
				{
					Inspect(Self);
				}
				const size_t Index = Pauses.size() - 1;
				if (Index < Commands.size())
				{
					Commands[Index](Self);
				}
				else
				{
					Self.Continue();
				}
			});
		}
	};

	const FScriptVariable* FindVariable(const std::vector<FScriptVariable>& Variables, const std::string& Name)
	{
		for (const FScriptVariable& Variable : Variables)
		{
			if (Variable.Name == Name)
			{
				return &Variable;
			}
		}
		return nullptr;
	}

	constexpr const char* GBreakpointScript = R"(local D = { Properties = { Speed = 5 } }
function D:OnStart()
	self.Count = 0
end
function D:OnUpdate(dt)
	local Doubled = self.Properties.Speed * 2
	local Info = { Name = "abc", Values = { 1, 2, 3 } }
	self.Count = self.Count + Doubled
end
return D
)";
} // namespace

E_TEST(ScriptDebugger_BreakpointLocalsAndWatch)
{
	WriteDebugScript("Scripts/Debug.lua", GBreakpointScript);
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Debug.lua");
	FDebugHarness Harness;
	Harness.Debugger.ToggleBreakpoint("scripts\\DEBUG.lua", 8); // 대소문자·구분자 무시

	bool        bInspected = false;
	std::string DoubledValue, InfoNameValue, ThirdValue, WatchValue, WatchError, SpeedValue;
	bool        bHasSelf = false, bHasDt = false;
	Harness.Inspect = [&](FScriptDebugger& Debugger) {
		if (bInspected)
		{
			return;
		}
		bInspected                                  = true;
		const int32                        Frame    = Debugger.GetPauseState().CurrentFrame;
		const std::vector<FScriptVariable> Locals   = Debugger.GetLocals(Frame);
		bHasSelf                                    = FindVariable(Locals, "self") != nullptr;
		bHasDt                                      = FindVariable(Locals, "dt") != nullptr;
		if (const FScriptVariable* Doubled = FindVariable(Locals, "Doubled"))
		{
			DoubledValue = Doubled->Value;
		}
		if (const FScriptVariable* Info = FindVariable(Locals, "Info"); Info != nullptr && Info->ChildHandle != 0)
		{
			const std::vector<FScriptVariable> Children = Debugger.GetChildren(Info->ChildHandle);
			if (const FScriptVariable* Name = FindVariable(Children, "Name"))
			{
				InfoNameValue = Name->Value;
			}
			if (const FScriptVariable* Values = FindVariable(Children, "Values"); Values != nullptr && Values->ChildHandle != 0)
			{
				const std::vector<FScriptVariable> Items = Debugger.GetChildren(Values->ChildHandle);
				if (Items.size() == 3 && Items[2].Name == "[3]")
				{
					ThirdValue = Items[2].Value;
				}
			}
		}
		FScriptVariable Watch;
		if (Debugger.Evaluate(Frame, "Doubled + self.Count + 1", Watch))
		{
			WatchValue = Watch.Value;
		}
		FScriptVariable Speed;
		if (Debugger.Evaluate(Frame, "self.Properties.Speed", Speed))
		{
			SpeedValue = Speed.Value;
		}
		FScriptVariable Bad;
		if (!Debugger.Evaluate(Frame, "NoSuchTable.Field", Bad))
		{
			WatchError = Bad.Value;
		}
	};

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.1f, nullptr);
	Scripts.Update(0.1f, nullptr);

	E_EXPECT_EQ(Harness.Pauses.size(), size_t(2));
	if (!Harness.Pauses.empty())
	{
		E_EXPECT_TRUE(Harness.Pauses[0].Reason == EScriptPauseReason::Breakpoint);
		E_EXPECT_EQ(Harness.Pauses[0].File, std::string("Scripts/Debug.lua"));
		E_EXPECT_EQ(Harness.Pauses[0].Line, 8);
		E_EXPECT_EQ(Harness.Pauses[0].TopFunction, std::string("self:OnUpdate")); // 엔진이 부른 메서드 이름 (self 클래스에서 찾음)
		E_EXPECT_FALSE(Harness.Pauses[0].bInCoroutine);
	}
	E_EXPECT_TRUE(bHasSelf && bHasDt);
	E_EXPECT_EQ(DoubledValue, std::string("10"));
	E_EXPECT_EQ(InfoNameValue, std::string("\"abc\""));
	E_EXPECT_EQ(ThirdValue, std::string("3"));
	E_EXPECT_EQ(WatchValue, std::string("11")); // 첫 정지: Count = 0
	E_EXPECT_EQ(SpeedValue, std::string("5"));
	E_EXPECT_TRUE(WatchError.find("NoSuchTable") != std::string::npos);
	E_EXPECT_EQ(Harness.Debugger.FindBreakpoint("Scripts/Debug.lua", 8)->HitCount, 2u);
	// 정지 처리기 밖에서는 조회 결과가 없다
	E_EXPECT_TRUE(Harness.Debugger.GetLocals(0).empty());
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('A'):GetScript().Count == 20)")); // 정지 후 계속 실행됨
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
	E_EXPECT_FALSE(Harness.Debugger.IsAttached());
}

E_TEST(ScriptDebugger_ConditionalBreakpoint)
{
	WriteDebugScript("Scripts/Debug.lua", GBreakpointScript);
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Debug.lua");
	FDebugHarness Harness;
	FScriptBreakpoint Breakpoint;
	Breakpoint.File      = "Scripts/Debug.lua";
	Breakpoint.Line      = 8;
	Breakpoint.Condition = "self.Count >= 20 and Doubled == 10";
	Harness.Debugger.AddBreakpoint(Breakpoint);

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	Scripts.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		Scripts.Update(0.1f, nullptr); // 8번 줄에서 Count = 0, 10, 20, 30
	}
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(2));

	// 조건식 오류는 멈추고 오류를 보여 준다
	Harness.Debugger.SetBreakpointCondition("Scripts/Debug.lua", 8, "Missing.Value > 1");
	Harness.Pauses.clear();
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(1));
	E_EXPECT_TRUE(!Harness.Pauses.empty() && Harness.Pauses[0].Message.find("조건식 오류") != std::string::npos);

	// 끈 중단점은 멈추지 않고 훅도 뗀다
	Harness.Debugger.SetBreakpointEnabled("Scripts/Debug.lua", 8, false);
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled());
	Harness.Pauses.clear();
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_TRUE(Harness.Pauses.empty());
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
}

E_TEST(ScriptDebugger_StepIntoOverOut)
{
	WriteDebugScript("Scripts/Step.lua", R"(local S = { Properties = {} }
local function Helper(x)
	local y = x + 1
	return y * 2
end
function S:OnUpdate(dt)
	local a = 1
	local b = Helper(a)
	local c = b + 1
	self.Result = c
end
return S
)");
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Step.lua");
	FDebugHarness Harness;
	Harness.Debugger.ToggleBreakpoint("Scripts/Step.lua", 8);
	Harness.Commands = {
		[](FScriptDebugger& D) { D.StepInto(); }, // 8 → Helper 3
		[](FScriptDebugger& D) { D.StepOver(); }, // 3 → 4
		[](FScriptDebugger& D) { D.StepOut(); },  // 4 → OnUpdate 9 (호출 줄로 돌아온 뒤 다음 줄)
		[](FScriptDebugger& D) { D.StepOver(); }, // 9 → 10
		[](FScriptDebugger& D) { D.Continue(); },
		// 두 번째 프레임: 8에서 Over는 Helper에 들어가지 않는다
		[](FScriptDebugger& D) { D.StepOver(); }, // 8 → 9
		[](FScriptDebugger& D) { D.Continue(); },
	};

	// Helper 안에서 멈췄을 때 바깥 프레임(OnUpdate)의 지역 변수와 upvalue(Helper 지역 함수)
	std::string OuterA, OuterUpvalue;
	Harness.Inspect = [&](FScriptDebugger& Debugger) {
		if (Harness.Pauses.size() != 2)
		{
			return;
		}
		if (const std::vector<FScriptVariable> Outer = Debugger.GetLocals(1); FindVariable(Outer, "a") != nullptr)
		{
			OuterA = FindVariable(Outer, "a")->Value;
		}
		if (const std::vector<FScriptVariable> Up = Debugger.GetUpvalues(1); FindVariable(Up, "Helper") != nullptr)
		{
			OuterUpvalue = FindVariable(Up, "Helper")->Type;
		}
	};

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.1f, nullptr);
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(OuterA, std::string("1"));
	E_EXPECT_EQ(OuterUpvalue, std::string("function"));
	Harness.Inspect = nullptr;

	const std::vector<int32> ExpectedLines = { 8, 3, 4, 9, 10, 8, 9 };
	E_EXPECT_EQ(Harness.Pauses.size(), ExpectedLines.size());
	for (size_t Index = 0; Index < Harness.Pauses.size() && Index < ExpectedLines.size(); ++Index)
	{
		E_EXPECT_EQ(Harness.Pauses[Index].Line, ExpectedLines[Index]);
	}
	if (Harness.Pauses.size() >= 4)
	{
		E_EXPECT_TRUE(Harness.Pauses[1].Reason == EScriptPauseReason::Step);
		E_EXPECT_EQ(Harness.Pauses[1].TopFunction, std::string("Helper"));
		E_EXPECT_EQ(Harness.Pauses[1].Depth, Harness.Pauses[0].Depth + 1);
		E_EXPECT_EQ(Harness.Pauses[3].Depth, Harness.Pauses[0].Depth);
	}
	E_EXPECT_TRUE(Scripts.RunString("assert(Scene.Find('A'):GetScript().Result == 5)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// 함수 끝에서 Over: 시작 프레임이 반환되면(호스트로) 다음 파일 줄에서 멈춘다 — 다음 프레임 OnUpdate 첫 줄
	Harness.Pauses.clear();
	Harness.Debugger.ClearBreakpoints();
	Harness.Debugger.ToggleBreakpoint("Scripts/Step.lua", 10);
	Harness.Commands = {
		[](FScriptDebugger& D) { D.StepOver(); }, // 10 → 11 (end)
		[](FScriptDebugger& D) { D.StepOver(); }, // 11 → (반환) → 다음 프레임 7
		[](FScriptDebugger& D) { D.Continue(); },
		[](FScriptDebugger& D) { D.Continue(); },
	};
	Scripts.Update(0.1f, nullptr);
	Harness.Debugger.ClearBreakpoints();
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(3));
	if (Harness.Pauses.size() == 3)
	{
		E_EXPECT_EQ(Harness.Pauses[0].Line, 10);
		E_EXPECT_EQ(Harness.Pauses[1].Line, 11);
		E_EXPECT_EQ(Harness.Pauses[2].Line, 7);
	}
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled()); // 단계 끝 + 중단점 없음 → 훅 해제
	Scripts.EndPlay();
}

E_TEST(ScriptDebugger_CoroutineStepping)
{
	WriteDebugScript("Scripts/Co.lua", R"(local C = { Properties = {} }
function C:OnStart()
	Coroutine.Start(function()
		local n = 1
		Wait()
		n = n + 1
		self.Done = n
		WaitFrames(2)
		self.Late = true
	end)
end
function C:OnUpdate(dt)
	self.Frames = (self.Frames or 0) + 1
end
return C
)");
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Co.lua");
	FDebugHarness Harness;
	Harness.Debugger.ToggleBreakpoint("Scripts/Co.lua", 4);
	Harness.Commands = {
		[](FScriptDebugger& D) { D.StepOver(); }, // 4 → 5
		[](FScriptDebugger& D) { D.StepOver(); }, // 5 (Wait — yield) → 다음 프레임 재개 6 (OnUpdate 줄은 건너뜀)
		[](FScriptDebugger& D) { D.Continue(); },
	};

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(2));
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(3));
	if (Harness.Pauses.size() == 3)
	{
		E_EXPECT_EQ(Harness.Pauses[0].Line, 4);
		E_EXPECT_TRUE(Harness.Pauses[0].bInCoroutine);
		E_EXPECT_EQ(Harness.Pauses[1].Line, 5);
		E_EXPECT_EQ(Harness.Pauses[2].Line, 6);
		E_EXPECT_TRUE(Harness.Pauses[2].bInCoroutine);
	}

	// 코루틴을 만든 뒤에 단 중단점도 그 코루틴에서 걸린다 (훅을 기존 스레드에도 건다)
	Harness.Debugger.ClearBreakpoints();
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled());
	Harness.Pauses.clear();
	Harness.Commands.clear();
	Harness.Debugger.ToggleBreakpoint("Scripts/Co.lua", 9);
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		Scripts.Update(0.1f, nullptr);
	}
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(1));
	E_EXPECT_TRUE(!Harness.Pauses.empty() && Harness.Pauses[0].Line == 9 && Harness.Pauses[0].bInCoroutine);
	E_EXPECT_TRUE(Scripts.RunString("local S = Scene.Find('A'):GetScript(); assert(S.Done == 2 and S.Late == true)"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
}

E_TEST(ScriptDebugger_BreakOnError)
{
	WriteDebugScript("Scripts/Fail.lua", R"(local F = { Properties = {} }
function F:OnUpdate(dt)
	local Value = nil
	local Sum = Value + 1
end
return F
)");
	WriteDebugScript("Scripts/CoFail.lua", R"(local C = { Properties = {} }
function C:OnStart()
	Coroutine.Start(function()
		local Step = 1
		Wait()
		Step = Step + 1
		error("코루틴 실패 " .. Step)
	end)
end
return C
)");
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Fail.lua");
	AddDebugScriptedEntity(Scene, "B", "Scripts/CoFail.lua");
	FDebugHarness Harness;
	bool          bValueLocalSeen = false;
	std::string   CoStepValue;
	Harness.Inspect = [&](FScriptDebugger& Debugger) {
		const FScriptPauseState&           State  = Debugger.GetPauseState();
		const std::vector<FScriptVariable> Locals = Debugger.GetLocals(State.CurrentFrame);
		if (!State.bInCoroutine)
		{
			const FScriptVariable* Value = FindVariable(Locals, "Value");
			bValueLocalSeen              = Value != nullptr && Value->Value == "nil";
		}
		else if (const FScriptVariable* Step = FindVariable(Locals, "Step"))
		{
			CoStepValue = Step->Value;
		}
		Debugger.StepOver(); // 오류 정지의 단계 명령은 계속과 같다
	};

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	E_EXPECT_TRUE(Harness.Debugger.GetBreakOnError());
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.1f, nullptr); // A 오류 정지 (B 코루틴은 Wait)
	Scripts.Update(0.1f, nullptr); // A는 멈춤(Faulted), B 코루틴 오류 정지
	Scripts.Update(0.1f, nullptr);

	E_EXPECT_EQ(Harness.Pauses.size(), size_t(2));
	if (Harness.Pauses.size() == 2)
	{
		E_EXPECT_TRUE(Harness.Pauses[0].Reason == EScriptPauseReason::Error);
		E_EXPECT_EQ(Harness.Pauses[0].File, std::string("Scripts/Fail.lua"));
		E_EXPECT_EQ(Harness.Pauses[0].Line, 4);
		E_EXPECT_TRUE(Harness.Pauses[0].Message.find("arithmetic") != std::string::npos);
		E_EXPECT_TRUE(Harness.Pauses[1].Reason == EScriptPauseReason::Error);
		E_EXPECT_TRUE(Harness.Pauses[1].bInCoroutine);
		E_EXPECT_EQ(Harness.Pauses[1].Line, 7);
		E_EXPECT_TRUE(Harness.Pauses[1].Message.find("코루틴 실패 2") != std::string::npos);
	}
	E_EXPECT_TRUE(bValueLocalSeen);
	E_EXPECT_EQ(CoStepValue, std::string("2"));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 2u); // 정지 후에도 오류는 원래대로 보고된다
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled());
	Scripts.EndPlay();

	// 끄면 멈추지 않는다
	Harness.Pauses.clear();
	Harness.Debugger.SetBreakOnError(false);
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_TRUE(Harness.Pauses.empty());
	Scripts.EndPlay();
}

E_TEST(ScriptDebugger_PauseRequestAndCoroutineLibrary)
{
	// coroutine.create/wrap 교체가 원래 동작(생성기, 오류 전파, 상태)을 유지하는지
	WriteDebugScript("Scripts/Gen.lua", R"(local G = { Properties = {} }
function G:OnUpdate(dt)
	local Gen = coroutine.wrap(function()
		for Index = 1, 3 do coroutine.yield(Index * 10) end
	end)
	self.Sum = Gen() + Gen() + Gen()
	local Co = coroutine.create(function(a) local b = coroutine.yield(a + 1); return b * 2 end)
	local _, First = coroutine.resume(Co, 1)
	local _, Second = coroutine.resume(Co, 5)
	self.Co = First + Second + (coroutine.status(Co) == "dead" and 100 or 0)
	local Ok, Message = pcall(coroutine.wrap(function() error("wrap 오류") end))
	self.WrapError = (not Ok) and tostring(Message):find("wrap 오류") ~= nil
end
return G
)");
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Gen.lua");
	FDebugHarness Harness;
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	Scripts.BeginPlay(Scene);
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled()); // 중단점이 없으면 훅 없음
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Debugger.GetLineHookCount(), uint64(0));
	E_EXPECT_TRUE(Harness.Pauses.empty()); // pcall로 잡은 오류는 멈추지 않는다
	E_EXPECT_TRUE(Scripts.RunString("local S = Scene.Find('A'):GetScript(); assert(S.Sum == 60, S.Sum); assert(S.Co == 112, S.Co); assert(S.WrapError)"));

	// 일시 정지 요청: 다음 파일 줄
	Harness.Debugger.RequestPause();
	E_EXPECT_TRUE(Harness.Debugger.AreHooksInstalled());
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(1));
	E_EXPECT_TRUE(!Harness.Pauses.empty() && Harness.Pauses[0].Reason == EScriptPauseReason::PauseRequest && Harness.Pauses[0].Line == 3);
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled());

	// 이번 세션 무시 (정지 중 플레이 정지): 중단점이 있어도 훅 없음, 다음 세션에서 풀림
	Harness.Debugger.ToggleBreakpoint("Scripts/Gen.lua", 3);
	Harness.Debugger.SuspendUntilNextSession();
	E_EXPECT_FALSE(Harness.Debugger.AreHooksInstalled());
	Harness.Pauses.clear();
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_TRUE(Harness.Pauses.empty());
	Scripts.EndPlay();
	Scripts.BeginPlay(Scene);
	E_EXPECT_TRUE(Harness.Debugger.AreHooksInstalled());
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(1));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
}

E_TEST(ScriptDebugger_BreakpointListOps)
{
	FScriptDebugger Debugger;
	E_EXPECT_EQ(FScriptDebugger::NormalizeFile(".\\Scripts\\Player.LUA"), std::string("scripts/player.lua"));
	E_EXPECT_TRUE(Debugger.ToggleBreakpoint("Scripts/A.lua", 3));
	E_EXPECT_TRUE(Debugger.ToggleBreakpoint("Scripts/A.lua", 30));
	E_EXPECT_TRUE(Debugger.ToggleBreakpoint("Scripts/B.lua", 5));
	E_EXPECT_FALSE(Debugger.ToggleBreakpoint("scripts/b.lua", 5)); // 같은 줄 = 제거
	E_EXPECT_EQ(Debugger.GetBreakpoints().size(), size_t(2));
	const uint32 Revision = Debugger.GetBreakpointRevision();
	E_EXPECT_EQ(Debugger.PruneBreakpoints("Scripts/A.lua", 10), 1); // 파일이 10줄로 줄면 30줄 중단점 제거
	E_EXPECT_EQ(Debugger.GetBreakpoints().size(), size_t(1));
	E_EXPECT_TRUE(Debugger.GetBreakpointRevision() != Revision);
	E_EXPECT_FALSE(Debugger.AreHooksInstalled()); // 연결 전에는 훅 없음
}

// 훅 비용 측정 (보고용 — 시간은 기록만 하고 판정은 훅 설치 여부로)
E_TEST(ScriptDebugger_HookCostMeasurement)
{
	WriteDebugScript("Scripts/Busy.lua", R"(local B = { Properties = {} }
function B:OnUpdate(dt)
	local Sum = 0
	for Index = 1, 200000 do
		Sum = Sum + Index % 7
	end
	self.Sum = Sum
end
return B
)");
	WriteDebugScript("Scripts/Other.lua", "local O = { Properties = {} }\nreturn O\n");
	FScene Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/Busy.lua");

	const auto Measure = [&](FScriptDebugger* Debugger) {
		FScriptSystem Scripts;
		Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
		Scripts.SetDebugger(Debugger);
		Scripts.BeginPlay(Scene);
		Scripts.Update(0.016f, nullptr); // 워밍업 (클래스 로드)
		const auto Start = std::chrono::steady_clock::now();
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			Scripts.Update(0.016f, nullptr);
		}
		const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count() / 10.0;
		Scripts.EndPlay();
		return Milliseconds;
	};

	const double NoDebugger = Measure(nullptr);
	FDebugHarness Idle;
	const double  IdleMs = Measure(&Idle.Debugger);
	E_EXPECT_EQ(Idle.Debugger.GetLineHookCount(), uint64(0)); // 중단점 없음 = 훅 없음
	FDebugHarness OtherFile;
	OtherFile.Debugger.ToggleBreakpoint("Scripts/Other.lua", 1);
	const double OtherFileMs = Measure(&OtherFile.Debugger);
	FDebugHarness SameFile;
	SameFile.Debugger.ToggleBreakpoint("Scripts/Busy.lua", 50); // 같은 파일, 실행되지 않는 줄 (줄마다 파일 일치 + 줄 비교 비용)
	const double SameFileMs = Measure(&SameFile.Debugger);
	// 동적 모드: 중단점 없는 파일의 줄은 이벤트 자체가 없다 (호출/반환 이벤트만), 중단점 파일 안에서만 줄 이벤트
	E_EXPECT_TRUE(OtherFile.Debugger.AreHooksInstalled() == false); // EndPlay 뒤 분리
	E_EXPECT_EQ(OtherFile.Debugger.GetLineHookCount(), uint64(0));
	E_EXPECT_TRUE(SameFile.Debugger.GetLineHookCount() > 0 && SameFile.Pauses.empty());
	E_LOG(LogScriptDebuggerTest, Display, "[스크립트 디버거 비용] 프레임당 200k 반복: 디버거 없음 {:.2f}ms / 연결·중단점 없음 {:.2f}ms / 다른 파일 중단점 {:.2f}ms / 같은 파일 중단점 {:.2f}ms",
	      NoDebugger, IdleMs, OtherFileMs, SameFileMs);
}

E_TEST(ScriptDebugger_BreakpointInRequiredModule)
{
	// 중단점이 있는 모듈 함수를 중단점 없는 스크립트가 부른다: 호출 이벤트가 그 함수 안에서만 줄 이벤트를 켠다
	WriteDebugScript("Scripts/MathLib.lua", R"(local M = {}
function M.Twice(Value)
	local Result = Value * 2
	return Result
end
return M
)");
	WriteDebugScript("Scripts/User.lua", R"(local MathLib = Script.Require("Scripts/MathLib.lua")
local U = { Properties = {} }
function U:OnUpdate(dt)
	local Total = 0
	for Index = 1, 50 do
		Total = Total + Index
	end
	self.Value = MathLib.Twice(Total)
end
return U
)");
	FScene        Scene;
	AddDebugScriptedEntity(Scene, "A", "Scripts/User.lua");
	FDebugHarness Harness;
	Harness.Debugger.ToggleBreakpoint("Scripts/MathLib.lua", 4);
	std::string ResultValue;
	Harness.Inspect = [&](FScriptDebugger& Debugger) {
		const std::vector<FScriptVariable> Locals = Debugger.GetLocals(Debugger.GetPauseState().CurrentFrame);
		if (const FScriptVariable* Result = FindVariable(Locals, "Result"))
		{
			ResultValue = Result->Value;
		}
	};
	Harness.Commands = {
		[](FScriptDebugger& D) { D.StepOut(); }, // MathLib 4 → User 9 (end)
		[](FScriptDebugger& D) { D.Continue(); },
	};
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(GetDebuggerTestContentDirectory());
	Scripts.SetDebugger(&Harness.Debugger);
	Scripts.BeginPlay(Scene);
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(2));
	if (Harness.Pauses.size() == 2)
	{
		E_EXPECT_EQ(Harness.Pauses[0].File, std::string("Scripts/MathLib.lua"));
		E_EXPECT_EQ(Harness.Pauses[0].Line, 4);
		E_EXPECT_EQ(Harness.Pauses[0].Depth, size_t(2)); // Twice ← OnUpdate
		E_EXPECT_EQ(Harness.Pauses[1].File, std::string("Scripts/User.lua"));
		E_EXPECT_EQ(Harness.Pauses[1].Line, 9);
	}
	E_EXPECT_EQ(ResultValue, std::string("2550"));
	// 단계가 끝나면 다시 동적 모드: User.lua의 반복문 줄은 이벤트가 없다 (MathLib 3·4줄만)
	const uint64 Before = Harness.Debugger.GetLineHookCount();
	Scripts.Update(0.1f, nullptr);
	E_EXPECT_EQ(Harness.Debugger.GetLineHookCount() - Before, uint64(2));
	E_EXPECT_EQ(Harness.Pauses.size(), size_t(3));
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	Scripts.EndPlay();
}
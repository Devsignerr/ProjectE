#include "Core/CommandLine.h"
#include "Core/Console/Console.h"
#include "Core/Testing/TestFramework.h"

#include <string>
#include <vector>

namespace
{
	// 명령 출력을 모으는 출력
	struct FCapture
	{
		std::vector<std::string> Lines;
		FConsoleOutput           Output{ [this](std::string_view Line) { Lines.emplace_back(Line); } };

		bool Contains(std::string_view Part) const
		{
			for (const std::string& Line : Lines)
			{
				if (Line.find(Part) != std::string::npos)
				{
					return true;
				}
			}
			return false;
		}
	};
} // namespace

E_TEST(Console_TokenizeAndAssignments)
{
	const std::vector<std::string> Tokens = ConsoleParsing::Tokenize("  r.Name  \"a b\"  c ");
	E_EXPECT_EQ(Tokens.size(), size_t(3));
	E_EXPECT_EQ(Tokens[0], std::string("r.Name"));
	E_EXPECT_EQ(Tokens[1], std::string("a b"));
	E_EXPECT_EQ(Tokens[2], std::string("c"));

	const auto List = ConsoleParsing::ParseAssignmentList("r.SSR=0, r.TAA = 1,,=5,r.Flag");
	E_EXPECT_EQ(List.size(), size_t(3));
	E_EXPECT_EQ(List[0].first, std::string("r.SSR"));
	E_EXPECT_EQ(List[0].second, std::string("0"));
	E_EXPECT_EQ(List[1].first, std::string("r.TAA"));
	E_EXPECT_EQ(List[1].second, std::string("1"));
	E_EXPECT_EQ(List[2].first, std::string("r.Flag"));
	E_EXPECT_TRUE(List[2].second.empty());
}

E_TEST(Console_ParseValues)
{
	bool B = false;
	E_EXPECT_TRUE(FConsoleVariable::ParseBool("On", B) && B);
	E_EXPECT_TRUE(FConsoleVariable::ParseBool(" false ", B) && !B);
	E_EXPECT_FALSE(FConsoleVariable::ParseBool("2", B));

	int32 I = 0;
	E_EXPECT_TRUE(FConsoleVariable::ParseInt("-3", {}, I) && I == -3);
	E_EXPECT_TRUE(FConsoleVariable::ParseInt("SSR", { "none", "ssr" }, I) && I == 1);
	E_EXPECT_FALSE(FConsoleVariable::ParseInt("1.5", {}, I));
	E_EXPECT_FALSE(FConsoleVariable::ParseInt("", {}, I));

	float F = 0.0f;
	E_EXPECT_TRUE(FConsoleVariable::ParseFloat("0.25", F));
	E_EXPECT_NEAR(F, 0.25f, 1e-6f);
	E_EXPECT_FALSE(FConsoleVariable::ParseFloat("abc", F));
}

E_TEST(Console_RegisterSetAndConvert)
{
	FConsoleManager   Console;
	FConsoleVariable& Bool   = Console.RegisterVariable("t.Bool", EConsoleVariableType::Bool, "1", "불");
	FConsoleVariable& Int    = Console.RegisterVariable("t.Int", EConsoleVariableType::Int, "2", "정수", EConsoleFlags::None,
	                                                    { .ValueNames = { "zero", "one", "two" }, .Range = std::pair(0.0f, 2.0f) });
	FConsoleVariable& Float  = Console.RegisterVariable("t.Float", EConsoleVariableType::Float, "0.5", "실수");
	FConsoleVariable& String = Console.RegisterVariable("t.String", EConsoleVariableType::String, "가", "문자열");

	E_EXPECT_TRUE(Bool.GetBool());
	E_EXPECT_EQ(Bool.GetInt(), 1);
	E_EXPECT_EQ(Int.GetInt(), 2);
	E_EXPECT_EQ(Int.GetDisplayString(), std::string("2 (two)"));
	E_EXPECT_NEAR(Float.GetFloat(), 0.5f, 1e-6f);
	E_EXPECT_EQ(String.GetString(), std::string("가"));

	// 찾기는 대소문자 무시
	E_EXPECT_TRUE(Console.FindVariable("T.BOOL") == &Bool);

	// 콘솔 줄로 설정 (이름 값, 범위 자르기, 문자열은 공백 포함)
	FCapture Capture;
	E_EXPECT_TRUE(Console.Execute("t.bool off", &Capture.Output));
	E_EXPECT_FALSE(Bool.GetBool());
	E_EXPECT_TRUE(Console.Execute("t.Int one", &Capture.Output));
	E_EXPECT_EQ(Int.GetInt(), 1);
	E_EXPECT_TRUE(Console.Execute("t.Int 9", &Capture.Output));
	E_EXPECT_EQ(Int.GetInt(), 2); // 범위 0~2
	E_EXPECT_FALSE(Console.Execute("t.Float abc", &Capture.Output));
	E_EXPECT_NEAR(Float.GetFloat(), 0.5f, 1e-6f);
	E_EXPECT_TRUE(Console.Execute("t.String 안녕 하세요", &Capture.Output));
	E_EXPECT_EQ(String.GetString(), std::string("안녕 하세요"));
	E_EXPECT_TRUE(Capture.Contains("] t.bool off")); // 입력 메아리

	// 값 보기 / 기본값 표시 / 초기화
	Capture.Lines.clear();
	E_EXPECT_TRUE(Console.Execute("t.Bool", &Capture.Output));
	E_EXPECT_TRUE(Capture.Contains("t.Bool = 0 (기본 1)"));
	Bool.Reset();
	E_EXPECT_TRUE(Bool.GetBool() && Bool.IsDefault());

	// 같은 이름 재등록은 기존 것
	FConsoleVariable& Again = Console.RegisterVariable("t.bool", EConsoleVariableType::Int, "5", "중복");
	E_EXPECT_TRUE(&Again == &Bool);

	// 모르는 이름
	E_EXPECT_FALSE(Console.Execute("t.Unknown 1", &Capture.Output));
}

E_TEST(Console_FlagsAndCallbacks)
{
	FConsoleManager   Console;
	FConsoleVariable& ReadOnly = Console.RegisterVariable("t.ReadOnly", EConsoleVariableType::Int, "1", "", EConsoleFlags::ReadOnly);
	FConsoleVariable& Cheat    = Console.RegisterVariable("t.Cheat", EConsoleVariableType::Bool, "0", "", EConsoleFlags::Cheat);
	FCapture          Capture;

	E_EXPECT_FALSE(Console.Execute("t.ReadOnly 5", &Capture.Output));
	E_EXPECT_EQ(ReadOnly.GetInt(), 1);
	E_EXPECT_TRUE(ReadOnly.Set("5", EConsoleSetBy::CommandLine)); // 명령줄/코드는 된다
	E_EXPECT_EQ(ReadOnly.GetInt(), 5);

	Console.SetCheatsAllowed(false);
	E_EXPECT_FALSE(Console.Execute("t.Cheat 1", &Capture.Output));
	E_EXPECT_FALSE(Cheat.GetBool());
	Console.SetCheatsAllowed(true);
	E_EXPECT_TRUE(Console.Execute("t.Cheat 1", &Capture.Output));
	E_EXPECT_TRUE(Cheat.GetBool());

	// 변경 콜백: 값이 바뀔 때만
	int32 Calls = 0;
	Cheat.AddOnChanged([&Calls](FConsoleVariable&) { ++Calls; });
	Cheat.SetBool(true);
	E_EXPECT_EQ(Calls, 0);
	Cheat.SetBool(false);
	E_EXPECT_EQ(Calls, 1);
	Console.Execute("t.Cheat 1", &Capture.Output);
	E_EXPECT_EQ(Calls, 2);
}

E_TEST(Console_CommandsAndBuiltins)
{
	FConsoleManager          Console;
	std::vector<std::string> Received;
	Console.RegisterCommand({ "t.Echo", "인자 받기", [&Received](const std::vector<std::string>& Args, const FConsoleOutput&) { Received = Args; }, { "alpha", "beta" } });

	FCapture Capture;
	E_EXPECT_TRUE(Console.Execute("t.echo 1 \"2 3\"", &Capture.Output));
	E_EXPECT_EQ(Received.size(), size_t(2));
	E_EXPECT_EQ(Received[1], std::string("2 3"));

	// 기본 명령
	E_EXPECT_TRUE(Console.Execute("help", &Capture.Output));
	E_EXPECT_TRUE(Capture.Contains("t.Echo"));
	Capture.Lines.clear();
	Console.RegisterVariable("t.Listed", EConsoleVariableType::Int, "3", "목록");
	E_EXPECT_TRUE(Console.Execute("cvars t.list", &Capture.Output));
	E_EXPECT_TRUE(Capture.Contains("t.Listed = 3"));
	E_EXPECT_TRUE(Capture.Contains("1개"));

	FConsoleVariable* StatFps = Console.FindVariable("stat.FPS");
	FConsoleVariable* StatGpu = Console.FindVariable("stat.GPU");
	E_EXPECT_TRUE(StatFps != nullptr && StatGpu != nullptr);
	E_EXPECT_TRUE(Console.Execute("stat fps", &Capture.Output));
	E_EXPECT_TRUE(StatFps->GetBool());
	E_EXPECT_TRUE(Console.Execute("stat gpu", &Capture.Output));
	E_EXPECT_TRUE(StatGpu->GetBool());
	E_EXPECT_TRUE(Console.Execute("stat none", &Capture.Output));
	E_EXPECT_FALSE(StatFps->GetBool() || StatGpu->GetBool());

	E_EXPECT_TRUE(Console.UnregisterCommand("t.echo"));
	E_EXPECT_FALSE(Console.Execute("t.Echo", &Capture.Output));
}

E_TEST(Console_Completions)
{
	FConsoleManager Console;
	Console.RegisterVariable("r.SSR", EConsoleVariableType::Bool, "1", "반사");
	Console.RegisterVariable("r.SSAO", EConsoleVariableType::Bool, "1", "차폐");
	Console.RegisterVariable("r.DebugView", EConsoleVariableType::Int, "0", "버퍼", EConsoleFlags::None, { .ValueNames = { "none", "normal", "ssr" } });
	Console.RegisterVariable("x.UsesSsr", EConsoleVariableType::Bool, "0", "포함 일치");

	// 접두사 일치(이름순) → 포함 일치
	const std::vector<FConsoleCompletion> Names = Console.GetCompletions("r.ss");
	E_EXPECT_EQ(Names.size(), size_t(2));
	E_EXPECT_EQ(Names[0].Text, std::string("r.SSAO"));
	E_EXPECT_EQ(Names[1].Text, std::string("r.SSR"));
	const std::vector<FConsoleCompletion> Contains = Console.GetCompletions("ssr");
	E_EXPECT_EQ(Contains.size(), size_t(2)); // 접두사 일치 없음 → 포함 일치 이름순
	E_EXPECT_EQ(Contains[0].Text, std::string("r.SSR"));
	E_EXPECT_EQ(Contains[1].Text, std::string("x.UsesSsr"));

	// 값 이름 / bool / 명령 인자
	const std::vector<FConsoleCompletion> Values = Console.GetCompletions("r.debugview n");
	E_EXPECT_EQ(Values.size(), size_t(2));
	E_EXPECT_EQ(Values[0].Text, std::string("r.DebugView none"));
	E_EXPECT_EQ(Console.GetCompletions("r.SSR ").size(), size_t(2));
	E_EXPECT_EQ(Console.GetCompletions("stat g").size(), size_t(1));

	// Tab: 공통 접두사 → 하나면 공백까지
	E_EXPECT_EQ(Console.CompleteInput("r.SS"), std::string("r.SS"));
	E_EXPECT_EQ(Console.CompleteInput("r.SSA"), std::string("r.SSAO "));
	E_EXPECT_EQ(Console.CompleteInput("r.d"), std::string("r.DebugView "));
	E_EXPECT_EQ(Console.CompleteInput("r.DebugView ss"), std::string("r.DebugView ssr "));
	E_EXPECT_EQ(Console.CompleteInput("zzz"), std::string("zzz"));
}

E_TEST(Console_CommandLine)
{
	const FCommandLine CommandLine = FCommandLine::Parse(L"--no-thing --level=4 --cvar t.A=7,t.Late=on --cvar t.B=2 --other");
	const std::vector<std::wstring> Values = CommandLine.GetValues(L"--cvar");
	E_EXPECT_EQ(Values.size(), size_t(2));

	FConsoleManager   Console;
	FConsoleVariable& Thing = Console.RegisterVariable("t.Thing", EConsoleVariableType::Bool, "1", "", EConsoleFlags::None,
	                                                   { .CommandLine = { { L"--no-thing", "0" } } });
	FConsoleVariable& Level = Console.RegisterVariable("t.Level", EConsoleVariableType::Int, "0", "", EConsoleFlags::None,
	                                                   { .CommandLine = { { L"--level", "" } } });
	FConsoleVariable& A     = Console.RegisterVariable("t.A", EConsoleVariableType::Int, "0", "", EConsoleFlags::None,
	                                                   { .CommandLine = { { L"--other", "3" } } });
	FConsoleVariable& B     = Console.RegisterVariable("t.B", EConsoleVariableType::Int, "0", "", EConsoleFlags::ReadOnly);
	Console.ApplyCommandLine(CommandLine);

	E_EXPECT_FALSE(Thing.GetBool());
	E_EXPECT_EQ(Level.GetInt(), 4);
	E_EXPECT_EQ(A.GetInt(), 7); // 별칭(3) 뒤에 --cvar가 이긴다
	E_EXPECT_EQ(B.GetInt(), 2); // 읽기 전용도 명령줄은 된다

	// 늦게 등록된 변수(게임 모듈)에도 적용
	FConsoleVariable& Late = Console.RegisterVariable("t.Late", EConsoleVariableType::Bool, "0", "");
	E_EXPECT_TRUE(Late.GetBool());
}

E_TEST(Console_History)
{
	FConsoleManager Console;
	Console.AddHistory("a");
	Console.AddHistory("b");
	Console.AddHistory(" a ");
	Console.AddHistory("");
	E_EXPECT_EQ(Console.GetHistory().size(), size_t(2));
	E_EXPECT_EQ(Console.GetHistory().back(), std::string("a"));
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Console.AddHistory(std::to_string(Index));
	}
	E_EXPECT_EQ(Console.GetHistory().size(), FConsoleManager::MaxHistory);
	E_EXPECT_EQ(Console.GetHistory().back(), std::string("99"));
}

E_TEST(Console_GlobalAutoVariable)
{
	// 정적 등록 헬퍼: 전역 레지스트리에 등록되고 소멸하면 빠진다
	{
		TAutoConsoleVariable<float> Variable("test.AutoFloat", 1.5f, "자동 등록");
		E_EXPECT_NEAR(Variable.Get(), 1.5f, 1e-6f);
		E_EXPECT_TRUE(FConsoleManager::Get().FindVariable("test.autofloat") != nullptr);
		Variable->SetFloat(2.0f);
		E_EXPECT_NEAR(Variable.Get(), 2.0f, 1e-6f);
	}
	E_EXPECT_TRUE(FConsoleManager::Get().FindVariable("test.AutoFloat") == nullptr);
}

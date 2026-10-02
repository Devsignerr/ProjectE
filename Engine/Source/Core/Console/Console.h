#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

class FCommandLine;
class FConsoleManager;

// ---- 콘솔 변수(CVar) / 명령 레지스트리 (언리얼 IConsoleManager식)
//   이름은 "r.SSR"처럼 점으로 나눈 영문, 찾기는 대소문자 무시. 값은 bool/int32/float/string 하나.
//   전역 레지스트리는 FConsoleManager::Get() 하나 (엔진 DLL 안 — 헤더 인라인 static 금지). 테스트는 자체 인스턴스를 만든다.
//   정적 등록: TAutoConsoleVariable / FAutoConsoleCommand (.cpp 전역). 등록한 모듈의 정적 소멸 때 해제되므로
//   게임 모듈/실행 파일에 둬도 안전하다 (엔진 DLL은 소비자보다 늦게 내려간다).
//   명령줄: --cvar r.SSR=0,r.TAA=0 (여러 번 가능) + 변수별 예전 플래그 별칭(--no-ssr 등). 늦게 등록된 변수(게임 모듈)에도 적용된다.
//   스레드: 메인 스레드 전용 (읽기 포함). 렌더러/게임 코드는 프레임마다 값을 읽어 반영한다.

E_DECLARE_ENGINE_LOG_CATEGORY(LogConsole)

enum class EConsoleVariableType : uint8
{
	Bool,
	Int,
	Float,
	String,
};

enum class EConsoleFlags : uint32
{
	None     = 0,
	ReadOnly = 1u << 0, // 콘솔 입력으로 바꿀 수 없다 (코드/명령줄만)
	Cheat    = 1u << 1, // 치트 허용(FConsoleManager::SetCheatsAllowed)일 때만 콘솔로 바꿀 수 있다
};
constexpr EConsoleFlags operator|(EConsoleFlags A, EConsoleFlags B) { return static_cast<EConsoleFlags>(static_cast<uint32>(A) | static_cast<uint32>(B)); }
constexpr bool          HasAnyFlags(EConsoleFlags Flags, EConsoleFlags Test) { return (static_cast<uint32>(Flags) & static_cast<uint32>(Test)) != 0; }

// 값을 바꾼 출처. Console만 ReadOnly/Cheat 검사를 받는다
enum class EConsoleSetBy : uint8
{
	Code,
	CommandLine,
	Console,
};

// 명령줄 별칭: Flag가 있으면 Value로 설정. Value가 비면 "--flag 값"의 값을 쓴다 (예: { L"--no-ssr", "0" }, { L"--force-lod" })
struct FConsoleCommandLineAlias
{
	std::wstring Flag;
	std::string  Value;
};

// 등록 옵션 (지정 초기화로 넘긴다)
struct FConsoleVariableDesc
{
	std::vector<std::string>              ValueNames; // 정수 변수: 번호 대신 쓸 수 있는 이름 (0부터 차례로, 자동 완성 후보)
	std::optional<std::pair<float, float>> Range;      // 정수/실수 변수: 설정 시 이 범위로 자른다
	std::vector<FConsoleCommandLineAlias> CommandLine;
};

class FConsoleVariable
{
public:
	using FChangedFn = std::function<void(FConsoleVariable&)>;

	FConsoleVariable(std::string_view InName, EConsoleVariableType InType, std::string_view InHelp, EConsoleFlags InFlags, FConsoleVariableDesc InDesc);

	const std::string&   GetName() const { return Name; }
	const std::string&   GetHelp() const { return Help; }
	EConsoleVariableType GetType() const { return Type; }
	EConsoleFlags        GetFlags() const { return Flags; }
	const FConsoleVariableDesc& GetDesc() const { return Desc; }

	// 다른 타입으로 읽으면 변환한다 (Bool ↔ 0/1, String은 파싱 — 실패하면 0)
	bool        GetBool() const;
	int32       GetInt() const;
	float       GetFloat() const;
	std::string GetString() const;        // 표시 문자열 (bool "1"/"0", 실수 최단 표기)
	std::string GetDefaultString() const;
	std::string GetDisplayString() const; // 이름 있는 정수는 "5 (ssr)"
	bool        IsDefault() const { return GetString() == GetDefaultString(); }

	// 문자열을 타입에 맞게 해석해 설정. 실패(형식, ReadOnly/Cheat)하면 false + OutError. 값이 바뀌면 변경 콜백
	bool Set(std::string_view Value, EConsoleSetBy SetBy = EConsoleSetBy::Code, std::string* OutError = nullptr);
	void SetBool(bool Value);
	void SetInt(int32 Value);
	void SetFloat(float Value);
	void SetString(std::string_view Value);
	void Reset();

	// 값이 바뀔 때마다 (리소스 재생성 등). 등록 순서대로 불린다
	void AddOnChanged(FChangedFn Callback) { Callbacks.push_back(std::move(Callback)); }

	// 해석 (순수 함수, 테스트용): 문자열 → 값. 실패하면 false
	static bool ParseBool(std::string_view Text, bool& Out);
	static bool ParseInt(std::string_view Text, const std::vector<std::string>& ValueNames, int32& Out);
	static bool ParseFloat(std::string_view Text, float& Out);

private:
	friend class FConsoleManager;

	// 새 값 적용 (콜백 포함). 같은 값이면 아무것도 하지 않는다
	void Assign(bool InBool, int32 InInt, float InFloat, std::string_view InString);

	std::string            Name;
	std::string            Help;
	EConsoleVariableType   Type;
	EConsoleFlags          Flags;
	FConsoleVariableDesc   Desc;
	FConsoleManager*       Owner = nullptr; // 치트 허용 확인

	bool        BoolValue  = false;
	int32       IntValue   = 0;
	float       FloatValue = 0.0f;
	std::string StringValue;
	std::string DefaultValue; // 표시 문자열

	std::vector<FChangedFn> Callbacks;
};

// 명령 출력: 줄 단위. Sink가 비면 LogConsole(Display)로
class FConsoleOutput
{
public:
	FConsoleOutput() = default;
	explicit FConsoleOutput(std::function<void(std::string_view)> InSink) : Sink(std::move(InSink)) {}

	void Print(std::string_view Line) const;
	template <typename... TArgs>
	void Printf(std::format_string<TArgs...> Format, TArgs&&... Args) const
	{
		Print(std::format(Format, std::forward<TArgs>(Args)...));
	}

private:
	std::function<void(std::string_view)> Sink;
};

// 명령: "이름 인자..." → 콜백 (인자는 공백 구분, "따옴표"로 묶기)
struct FConsoleCommand
{
	using FExecuteFn = std::function<void(const std::vector<std::string>& Args, const FConsoleOutput& Output)>;

	std::string              Name;
	std::string              Help;
	FExecuteFn               Execute;
	std::vector<std::string> ArgumentNames; // 첫 인자 자동 완성 후보 (예: stat → fps, gpu, none)
	EConsoleFlags            Flags = EConsoleFlags::None; // Cheat만 의미 있음
};

// 자동 완성 후보 하나
struct FConsoleCompletion
{
	std::string Text;   // 고르면 입력 줄이 될 전체 문자열 (예: "r.DebugView ssr")
	std::string Name;   // 표시 이름 (변수/명령 이름 또는 값 이름)
	std::string Detail; // 현재 값 / 도움말 한 줄
	bool        bIsCommand = false;
};

class FConsoleManager
{
public:
	static FConsoleManager& Get(); // 엔진 DLL 전역 하나

	FConsoleManager();  // 기본 명령(help, cvars, stat)과 stat.* 변수를 등록한다
	~FConsoleManager();
	FConsoleManager(const FConsoleManager&)            = delete;
	FConsoleManager& operator=(const FConsoleManager&) = delete;

	// 등록. 같은 이름이 이미 있으면 경고 후 기존 것을 돌려준다 (Default는 표시 문자열 — 타입에 맞게 해석)
	FConsoleVariable& RegisterVariable(std::string_view Name, EConsoleVariableType Type, std::string_view Default, std::string_view Help,
	                                   EConsoleFlags Flags = EConsoleFlags::None, FConsoleVariableDesc Desc = {});
	bool              UnregisterVariable(std::string_view Name);
	FConsoleCommand&  RegisterCommand(FConsoleCommand Command);
	bool              UnregisterCommand(std::string_view Name);

	FConsoleVariable*       FindVariable(std::string_view Name);
	const FConsoleVariable* FindVariable(std::string_view Name) const;
	const FConsoleCommand*  FindCommand(std::string_view Name) const;
	// 이름순 (cvars/help 출력, 테스트)
	std::vector<const FConsoleVariable*> GetVariables() const;
	std::vector<const FConsoleCommand*>  GetCommands() const;

	// 한 줄 실행: "r.SSR 0"(설정), "r.SSR"(값 보기), "r.SSR ?"(도움말), "stat fps"(명령). 출력 첫 줄은 "] 입력" 메아리.
	// 반환: 알려진 이름이고 성공했으면 true. Output이 null이면 로그로
	bool Execute(std::string_view Line, const FConsoleOutput* Output = nullptr);

	// 자동 완성: 첫 단어(이름) — 접두사 일치 먼저(이름순), 그다음 포함 일치. 공백 뒤 — 그 변수의 값 이름/명령의 인자 후보
	std::vector<FConsoleCompletion> GetCompletions(std::string_view Input, size_t MaxCount = 32) const;
	// Tab: 후보들의 공통 접두사까지 채운다 (후보가 하나면 그것 + 공백). 바뀔 게 없으면 Input 그대로
	std::string CompleteInput(std::string_view Input) const;

	// 명령줄 적용: 변수 별칭 → --cvar 목록 순서. 이후 등록되는 변수에도 같은 규칙을 적용하도록 기억한다
	void ApplyCommandLine(const FCommandLine& CommandLine);

	void SetCheatsAllowed(bool bAllowed) { bCheatsAllowed = bAllowed; }
	bool AreCheatsAllowed() const { return bCheatsAllowed; }

	// 입력 기록 (에디터/런타임 콘솔 공용, 최근 MaxHistory개, 연속 중복 제외)
	static constexpr size_t         MaxHistory = 64;
	void                            AddHistory(std::string_view Line);
	const std::vector<std::string>& GetHistory() const { return History; }

private:
	static std::string ToKey(std::string_view Name);
	void               ApplyCommandLineTo(FConsoleVariable& Variable);
	void               RegisterBuiltins();

	std::unordered_map<std::string, std::unique_ptr<FConsoleVariable>> Variables; // 키: 소문자 이름
	std::unordered_map<std::string, FConsoleCommand>                   Commands;
	bool                                                               bCheatsAllowed = true;
	std::vector<std::string>                                           History;

	// 명령줄 (ApplyCommandLine 이후 등록되는 변수용)
	struct FStoredCommandLine
	{
		std::vector<std::pair<std::wstring, std::wstring>> Flags; // (플래그, 값) — 별칭 확인용 원본 인자
		std::vector<std::pair<std::string, std::string>>   Assignments; // --cvar 목록 (순서대로)
	};
	std::optional<FStoredCommandLine> StoredCommandLine;
};

// ---- 순수 해석 함수 (테스트 대상)
namespace ConsoleParsing
{
	// 공백 구분, "따옴표"로 묶은 토큰은 공백 포함 (따옴표 제거)
	std::vector<std::string> Tokenize(std::string_view Line);
	// "a=1,b=2" → {(a,1), (b,2)}. 이름이 빈 항목은 건너뛴다
	std::vector<std::pair<std::string, std::string>> ParseAssignmentList(std::string_view List);
	bool EqualsIgnoreCase(std::string_view A, std::string_view B);
	bool StartsWithIgnoreCase(std::string_view Text, std::string_view Prefix);
	bool ContainsIgnoreCase(std::string_view Text, std::string_view Part);
} // namespace ConsoleParsing

// ---- 정적 등록 헬퍼
namespace ConsoleDetail
{
	// 소멸 시 해제 (엔진 DLL이 이미 내려갔으면 아무것도 하지 않는다 — 실제로는 소비자가 먼저 내려간다)
	void UnregisterVariableIfAlive(std::string_view Name);
	void UnregisterCommandIfAlive(std::string_view Name);

	template <typename T>
	constexpr EConsoleVariableType GetVariableType()
	{
		if constexpr (std::is_same_v<T, bool>)
		{
			return EConsoleVariableType::Bool;
		}
		else if constexpr (std::is_same_v<T, int32>)
		{
			return EConsoleVariableType::Int;
		}
		else if constexpr (std::is_same_v<T, float>)
		{
			return EConsoleVariableType::Float;
		}
		else
		{
			static_assert(std::is_same_v<T, std::string>, "콘솔 변수 타입은 bool/int32/float/std::string");
			return EConsoleVariableType::String;
		}
	}

	std::string ToDefaultString(bool Value);
	std::string ToDefaultString(int32 Value);
	std::string ToDefaultString(float Value);
	std::string ToDefaultString(const std::string& Value);
} // namespace ConsoleDetail

// 전역 레지스트리에 등록되는 콘솔 변수 (.cpp 전역 또는 함수 정적). Get()은 포인터 하나 따라가기
template <typename T>
class TAutoConsoleVariable
{
public:
	TAutoConsoleVariable(std::string_view Name, const T& Default, std::string_view Help, EConsoleFlags Flags = EConsoleFlags::None,
	                     FConsoleVariableDesc Desc = {})
		: Variable(&FConsoleManager::Get().RegisterVariable(Name, ConsoleDetail::GetVariableType<T>(), ConsoleDetail::ToDefaultString(Default), Help,
		                                                    Flags, std::move(Desc)))
		, VariableName(Name)
	{
	}
	~TAutoConsoleVariable() { ConsoleDetail::UnregisterVariableIfAlive(VariableName); }
	TAutoConsoleVariable(const TAutoConsoleVariable&)            = delete;
	TAutoConsoleVariable& operator=(const TAutoConsoleVariable&) = delete;

	T Get() const
	{
		if constexpr (std::is_same_v<T, bool>)
		{
			return Variable->GetBool();
		}
		else if constexpr (std::is_same_v<T, int32>)
		{
			return Variable->GetInt();
		}
		else if constexpr (std::is_same_v<T, float>)
		{
			return Variable->GetFloat();
		}
		else
		{
			return Variable->GetString();
		}
	}
	FConsoleVariable* operator->() const { return Variable; }
	FConsoleVariable& operator*() const { return *Variable; }

private:
	FConsoleVariable* Variable; // 비소유 (전역 레지스트리가 소유, 이 객체가 소멸할 때 해제)
	std::string       VariableName;
};

// 전역 레지스트리에 등록되는 명령 (.cpp 전역)
class FAutoConsoleCommand
{
public:
	FAutoConsoleCommand(std::string_view Name, std::string_view Help, FConsoleCommand::FExecuteFn Execute, std::vector<std::string> ArgumentNames = {},
	                    EConsoleFlags Flags = EConsoleFlags::None);
	~FAutoConsoleCommand();
	FAutoConsoleCommand(const FAutoConsoleCommand&)            = delete;
	FAutoConsoleCommand& operator=(const FAutoConsoleCommand&) = delete;

private:
	std::string CommandName;
};

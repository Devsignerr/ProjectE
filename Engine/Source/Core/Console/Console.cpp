#include "Core/Console/Console.h"

#include "Core/CommandLine.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

E_DEFINE_LOG_CATEGORY(LogConsole, Log)

namespace
{
	// 전역 레지스트리 수명 표시 (트리비얼 타입이라 DLL이 내려갈 때까지 메모리가 유효 — 소비자의 정적 소멸에서 확인)
	constinit bool GGlobalConsoleAlive = false;

	struct FGlobalConsoleHolder
	{
		FConsoleManager Manager;
		FGlobalConsoleHolder() { GGlobalConsoleAlive = true; }
		~FGlobalConsoleHolder() { GGlobalConsoleAlive = false; }
	};

	char ToLowerAscii(char Char)
	{
		return Char >= 'A' && Char <= 'Z' ? static_cast<char>(Char - 'A' + 'a') : Char;
	}

	std::string_view TrimView(std::string_view Text)
	{
		while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
		{
			Text.remove_prefix(1);
		}
		while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
		{
			Text.remove_suffix(1);
		}
		return Text;
	}

	std::string FormatFloat(float Value)
	{
		return std::format("{}", Value); // 최단 왕복 표기 (0.1 → "0.1")
	}

	const char* GetTypeName(EConsoleVariableType Type)
	{
		switch (Type)
		{
		case EConsoleVariableType::Bool:   return "bool";
		case EConsoleVariableType::Int:    return "int";
		case EConsoleVariableType::Float:  return "float";
		case EConsoleVariableType::String: return "string";
		}
		return "?";
	}

	// 도움말 첫 줄 (자동 완성 목록용)
	std::string FirstLine(const std::string& Text)
	{
		const size_t End = Text.find('\n');
		return End == std::string::npos ? Text : Text.substr(0, End);
	}
} // namespace

// ---------------------------------------------------------------- ConsoleParsing

namespace ConsoleParsing
{
	std::vector<std::string> Tokenize(std::string_view Line)
	{
		std::vector<std::string> Tokens;
		size_t                   Index = 0;
		while (Index < Line.size())
		{
			while (Index < Line.size() && std::isspace(static_cast<unsigned char>(Line[Index])))
			{
				++Index;
			}
			if (Index >= Line.size())
			{
				break;
			}
			std::string Token;
			if (Line[Index] == '"')
			{
				++Index;
				while (Index < Line.size() && Line[Index] != '"')
				{
					Token.push_back(Line[Index++]);
				}
				++Index; // 닫는 따옴표 (없으면 끝까지)
			}
			else
			{
				while (Index < Line.size() && !std::isspace(static_cast<unsigned char>(Line[Index])))
				{
					Token.push_back(Line[Index++]);
				}
			}
			Tokens.push_back(std::move(Token));
		}
		return Tokens;
	}

	std::vector<std::pair<std::string, std::string>> ParseAssignmentList(std::string_view List)
	{
		std::vector<std::pair<std::string, std::string>> Result;
		size_t                                           Begin = 0;
		while (Begin <= List.size())
		{
			const size_t           End  = std::min(List.find(',', Begin), List.size());
			const std::string_view Item = TrimView(List.substr(Begin, End - Begin));
			const size_t           Eq   = Item.find('=');
			const std::string_view Name = TrimView(Item.substr(0, Eq));
			if (!Name.empty())
			{
				const std::string_view Value = Eq == std::string_view::npos ? std::string_view() : TrimView(Item.substr(Eq + 1));
				Result.emplace_back(std::string(Name), std::string(Value));
			}
			Begin = End + 1;
		}
		return Result;
	}

	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin(), [](char X, char Y) { return ToLowerAscii(X) == ToLowerAscii(Y); });
	}

	bool StartsWithIgnoreCase(std::string_view Text, std::string_view Prefix)
	{
		return Text.size() >= Prefix.size() && EqualsIgnoreCase(Text.substr(0, Prefix.size()), Prefix);
	}

	bool ContainsIgnoreCase(std::string_view Text, std::string_view Part)
	{
		if (Part.empty())
		{
			return true;
		}
		for (size_t Index = 0; Index + Part.size() <= Text.size(); ++Index)
		{
			if (EqualsIgnoreCase(Text.substr(Index, Part.size()), Part))
			{
				return true;
			}
		}
		return false;
	}
} // namespace ConsoleParsing

// ---------------------------------------------------------------- FConsoleVariable

FConsoleVariable::FConsoleVariable(std::string_view InName, EConsoleVariableType InType, std::string_view InHelp, EConsoleFlags InFlags,
                                   FConsoleVariableDesc InDesc)
	: Name(InName)
	, Help(InHelp)
	, Type(InType)
	, Flags(InFlags)
	, Desc(std::move(InDesc))
{
}

bool FConsoleVariable::ParseBool(std::string_view Text, bool& Out)
{
	Text = TrimView(Text);
	using ConsoleParsing::EqualsIgnoreCase;
	if (Text == "1" || EqualsIgnoreCase(Text, "true") || EqualsIgnoreCase(Text, "on") || EqualsIgnoreCase(Text, "yes"))
	{
		Out = true;
		return true;
	}
	if (Text == "0" || EqualsIgnoreCase(Text, "false") || EqualsIgnoreCase(Text, "off") || EqualsIgnoreCase(Text, "no"))
	{
		Out = false;
		return true;
	}
	return false;
}

bool FConsoleVariable::ParseInt(std::string_view Text, const std::vector<std::string>& ValueNames, int32& Out)
{
	Text = TrimView(Text);
	for (size_t Index = 0; Index < ValueNames.size(); ++Index)
	{
		if (ConsoleParsing::EqualsIgnoreCase(Text, ValueNames[Index]))
		{
			Out = static_cast<int32>(Index);
			return true;
		}
	}
	int32       Value  = 0;
	const char* Begin  = Text.data();
	const char* End    = Text.data() + Text.size();
	const auto  Result = std::from_chars(Begin, End, Value);
	if (Result.ec != std::errc() || Result.ptr != End || Text.empty())
	{
		return false;
	}
	Out = Value;
	return true;
}

bool FConsoleVariable::ParseFloat(std::string_view Text, float& Out)
{
	Text = TrimView(Text);
	float       Value  = 0.0f;
	const char* End    = Text.data() + Text.size();
	const auto  Result = std::from_chars(Text.data(), End, Value);
	if (Result.ec != std::errc() || Result.ptr != End || Text.empty() || !std::isfinite(Value))
	{
		return false;
	}
	Out = Value;
	return true;
}

bool FConsoleVariable::GetBool() const
{
	switch (Type)
	{
	case EConsoleVariableType::Bool:  return BoolValue;
	case EConsoleVariableType::Int:   return IntValue != 0;
	case EConsoleVariableType::Float: return FloatValue != 0.0f;
	case EConsoleVariableType::String:
	{
		bool Value = false;
		return ParseBool(StringValue, Value) && Value;
	}
	}
	return false;
}

int32 FConsoleVariable::GetInt() const
{
	switch (Type)
	{
	case EConsoleVariableType::Bool:  return BoolValue ? 1 : 0;
	case EConsoleVariableType::Int:   return IntValue;
	case EConsoleVariableType::Float: return static_cast<int32>(FloatValue);
	case EConsoleVariableType::String:
	{
		int32 Value = 0;
		return ParseInt(StringValue, {}, Value) ? Value : 0;
	}
	}
	return 0;
}

float FConsoleVariable::GetFloat() const
{
	switch (Type)
	{
	case EConsoleVariableType::Bool:  return BoolValue ? 1.0f : 0.0f;
	case EConsoleVariableType::Int:   return static_cast<float>(IntValue);
	case EConsoleVariableType::Float: return FloatValue;
	case EConsoleVariableType::String:
	{
		float Value = 0.0f;
		return ParseFloat(StringValue, Value) ? Value : 0.0f;
	}
	}
	return 0.0f;
}

std::string FConsoleVariable::GetString() const
{
	switch (Type)
	{
	case EConsoleVariableType::Bool:   return BoolValue ? "1" : "0";
	case EConsoleVariableType::Int:    return std::to_string(IntValue);
	case EConsoleVariableType::Float:  return FormatFloat(FloatValue);
	case EConsoleVariableType::String: return StringValue;
	}
	return {};
}

std::string FConsoleVariable::GetDefaultString() const
{
	return DefaultValue;
}

std::string FConsoleVariable::GetDisplayString() const
{
	if (Type == EConsoleVariableType::Int && IntValue >= 0 && static_cast<size_t>(IntValue) < Desc.ValueNames.size())
	{
		return std::format("{} ({})", IntValue, Desc.ValueNames[static_cast<size_t>(IntValue)]);
	}
	if (Type == EConsoleVariableType::String)
	{
		return std::format("\"{}\"", StringValue);
	}
	return GetString();
}

void FConsoleVariable::Assign(bool InBool, int32 InInt, float InFloat, std::string_view InString)
{
	bool bChanged = false;
	switch (Type)
	{
	case EConsoleVariableType::Bool:
		bChanged  = BoolValue != InBool;
		BoolValue = InBool;
		break;
	case EConsoleVariableType::Int:
		if (Desc.Range)
		{
			InInt = std::clamp(InInt, static_cast<int32>(std::ceil(Desc.Range->first)), static_cast<int32>(std::floor(Desc.Range->second)));
		}
		bChanged = IntValue != InInt;
		IntValue = InInt;
		break;
	case EConsoleVariableType::Float:
		if (Desc.Range)
		{
			InFloat = std::clamp(InFloat, Desc.Range->first, Desc.Range->second);
		}
		bChanged   = FloatValue != InFloat;
		FloatValue = InFloat;
		break;
	case EConsoleVariableType::String:
		bChanged    = StringValue != InString;
		StringValue = std::string(InString);
		break;
	}
	if (bChanged)
	{
		for (const FChangedFn& Callback : Callbacks)
		{
			Callback(*this);
		}
	}
}

bool FConsoleVariable::Set(std::string_view Value, EConsoleSetBy SetBy, std::string* OutError)
{
	const auto Fail = [OutError](std::string Message) {
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	if (SetBy == EConsoleSetBy::Console)
	{
		if (HasAnyFlags(Flags, EConsoleFlags::ReadOnly))
		{
			return Fail(std::format("{}은(는) 읽기 전용입니다 (명령줄 --cvar로만 바꿀 수 있음)", Name));
		}
		if (HasAnyFlags(Flags, EConsoleFlags::Cheat) && Owner != nullptr && !Owner->AreCheatsAllowed())
		{
			return Fail(std::format("{}은(는) 치트 변수입니다 (치트가 꺼져 있음)", Name));
		}
	}

	switch (Type)
	{
	case EConsoleVariableType::Bool:
	{
		bool Parsed = false;
		if (!ParseBool(Value, Parsed))
		{
			return Fail(std::format("{}: bool 값이 아닙니다 '{}' (1/0, true/false, on/off)", Name, Value));
		}
		Assign(Parsed, 0, 0.0f, {});
		return true;
	}
	case EConsoleVariableType::Int:
	{
		int32 Parsed = 0;
		if (!ParseInt(Value, Desc.ValueNames, Parsed))
		{
			std::string Names;
			for (const std::string& ValueName : Desc.ValueNames)
			{
				Names += (Names.empty() ? " (" : "|") + ValueName;
			}
			return Fail(std::format("{}: 정수 값이 아닙니다 '{}'{}", Name, Value, Names.empty() ? "" : Names + ")"));
		}
		Assign(false, Parsed, 0.0f, {});
		return true;
	}
	case EConsoleVariableType::Float:
	{
		float Parsed = 0.0f;
		if (!ParseFloat(Value, Parsed))
		{
			return Fail(std::format("{}: 실수 값이 아닙니다 '{}'", Name, Value));
		}
		Assign(false, 0, Parsed, {});
		return true;
	}
	case EConsoleVariableType::String:
		Assign(false, 0, 0.0f, Value);
		return true;
	}
	return false;
}

void FConsoleVariable::SetBool(bool Value)
{
	Assign(Value, Value ? 1 : 0, Value ? 1.0f : 0.0f, Value ? "1" : "0");
}

void FConsoleVariable::SetInt(int32 Value)
{
	if (Type == EConsoleVariableType::String)
	{
		Assign(false, 0, 0.0f, std::to_string(Value));
		return;
	}
	Assign(Value != 0, Value, static_cast<float>(Value), {});
}

void FConsoleVariable::SetFloat(float Value)
{
	if (Type == EConsoleVariableType::String)
	{
		Assign(false, 0, 0.0f, FormatFloat(Value));
		return;
	}
	Assign(Value != 0.0f, static_cast<int32>(Value), Value, {});
}

void FConsoleVariable::SetString(std::string_view Value)
{
	Set(Value, EConsoleSetBy::Code);
}

void FConsoleVariable::Reset()
{
	Set(DefaultValue, EConsoleSetBy::Code);
}

// ---------------------------------------------------------------- FConsoleOutput

void FConsoleOutput::Print(std::string_view Line) const
{
	if (Sink)
	{
		Sink(Line);
	}
	else
	{
		E_LOG(LogConsole, Display, "{}", Line);
	}
}

// ---------------------------------------------------------------- 정적 등록 헬퍼

namespace ConsoleDetail
{
	void UnregisterVariableIfAlive(std::string_view Name)
	{
		if (GGlobalConsoleAlive)
		{
			FConsoleManager::Get().UnregisterVariable(Name);
		}
	}

	void UnregisterCommandIfAlive(std::string_view Name)
	{
		if (GGlobalConsoleAlive)
		{
			FConsoleManager::Get().UnregisterCommand(Name);
		}
	}

	std::string ToDefaultString(bool Value) { return Value ? "1" : "0"; }
	std::string ToDefaultString(int32 Value) { return std::to_string(Value); }
	std::string ToDefaultString(float Value) { return FormatFloat(Value); }
	std::string ToDefaultString(const std::string& Value) { return Value; }
} // namespace ConsoleDetail

FAutoConsoleCommand::FAutoConsoleCommand(std::string_view Name, std::string_view Help, FConsoleCommand::FExecuteFn Execute,
                                         std::vector<std::string> ArgumentNames, EConsoleFlags Flags)
	: CommandName(Name)
{
	FConsoleManager::Get().RegisterCommand({ std::string(Name), std::string(Help), std::move(Execute), std::move(ArgumentNames), Flags });
}

FAutoConsoleCommand::~FAutoConsoleCommand()
{
	ConsoleDetail::UnregisterCommandIfAlive(CommandName);
}

// ---------------------------------------------------------------- FConsoleManager

FConsoleManager& FConsoleManager::Get()
{
	static FGlobalConsoleHolder Holder; // 엔진 DLL 안 하나
	return Holder.Manager;
}

FConsoleManager::FConsoleManager()
{
	RegisterBuiltins();
}

FConsoleManager::~FConsoleManager() = default;

std::string FConsoleManager::ToKey(std::string_view Name)
{
	std::string Key(Name);
	std::transform(Key.begin(), Key.end(), Key.begin(), ToLowerAscii);
	return Key;
}

FConsoleVariable& FConsoleManager::RegisterVariable(std::string_view Name, EConsoleVariableType Type, std::string_view Default, std::string_view Help,
                                                    EConsoleFlags Flags, FConsoleVariableDesc Desc)
{
	const std::string Key = ToKey(Name);
	if (const auto Found = Variables.find(Key); Found != Variables.end())
	{
		E_LOG(LogConsole, Warning, "콘솔 변수 '{}'가 이미 등록되어 있습니다 (기존 것을 씁니다)", Name);
		return *Found->second;
	}
	if (Commands.contains(Key))
	{
		E_LOG(LogConsole, Warning, "콘솔 변수 '{}'가 같은 이름의 명령을 가립니다", Name);
	}

	auto Variable   = std::make_unique<FConsoleVariable>(Name, Type, Help, Flags, std::move(Desc));
	Variable->Owner = this;
	if (!Variable->Set(Default, EConsoleSetBy::Code))
	{
		E_LOG(LogConsole, Warning, "콘솔 변수 '{}' 기본값 '{}'을(를) 해석하지 못했습니다", Name, Default);
	}
	Variable->DefaultValue = Variable->GetString();
	FConsoleVariable& Result = *Variable;
	Variables.emplace(Key, std::move(Variable));
	ApplyCommandLineTo(Result);
	return Result;
}

bool FConsoleManager::UnregisterVariable(std::string_view Name)
{
	return Variables.erase(ToKey(Name)) > 0;
}

FConsoleCommand& FConsoleManager::RegisterCommand(FConsoleCommand Command)
{
	const std::string Key = ToKey(Command.Name);
	if (Commands.contains(Key))
	{
		E_LOG(LogConsole, Warning, "콘솔 명령 '{}'를 다시 등록합니다 (덮어씀)", Command.Name);
	}
	FConsoleCommand& Stored = Commands[Key];
	Stored                  = std::move(Command);
	return Stored;
}

bool FConsoleManager::UnregisterCommand(std::string_view Name)
{
	return Commands.erase(ToKey(Name)) > 0;
}

FConsoleVariable* FConsoleManager::FindVariable(std::string_view Name)
{
	const auto Found = Variables.find(ToKey(Name));
	return Found != Variables.end() ? Found->second.get() : nullptr;
}

const FConsoleVariable* FConsoleManager::FindVariable(std::string_view Name) const
{
	const auto Found = Variables.find(ToKey(Name));
	return Found != Variables.end() ? Found->second.get() : nullptr;
}

const FConsoleCommand* FConsoleManager::FindCommand(std::string_view Name) const
{
	const auto Found = Commands.find(ToKey(Name));
	return Found != Commands.end() ? &Found->second : nullptr;
}

std::vector<const FConsoleVariable*> FConsoleManager::GetVariables() const
{
	std::vector<const FConsoleVariable*> Result;
	Result.reserve(Variables.size());
	for (const auto& [Key, Variable] : Variables)
	{
		Result.push_back(Variable.get());
	}
	std::sort(Result.begin(), Result.end(), [](const FConsoleVariable* A, const FConsoleVariable* B) { return ToKey(A->GetName()) < ToKey(B->GetName()); });
	return Result;
}

std::vector<const FConsoleCommand*> FConsoleManager::GetCommands() const
{
	std::vector<const FConsoleCommand*> Result;
	Result.reserve(Commands.size());
	for (const auto& [Key, Command] : Commands)
	{
		Result.push_back(&Command);
	}
	std::sort(Result.begin(), Result.end(), [](const FConsoleCommand* A, const FConsoleCommand* B) { return ToKey(A->Name) < ToKey(B->Name); });
	return Result;
}

bool FConsoleManager::Execute(std::string_view Line, const FConsoleOutput* InOutput)
{
	const FConsoleOutput  DefaultOutput;
	const FConsoleOutput& Output = InOutput != nullptr ? *InOutput : DefaultOutput;

	const std::string_view Trimmed = TrimView(Line);
	if (Trimmed.empty())
	{
		return false;
	}
	Output.Printf("] {}", Trimmed);

	std::vector<std::string> Tokens = ConsoleParsing::Tokenize(Trimmed);
	if (Tokens.empty())
	{
		return false;
	}
	const std::string Name = Tokens.front();
	Tokens.erase(Tokens.begin());

	if (FConsoleVariable* Variable = FindVariable(Name))
	{
		if (Tokens.empty())
		{
			Output.Printf("{} = {}{}", Variable->GetName(), Variable->GetDisplayString(), Variable->IsDefault() ? "" : std::format(" (기본 {})", Variable->GetDefaultString()));
			return true;
		}
		if (Tokens.front() == "?")
		{
			Output.Printf("{} ({}) = {} — {}", Variable->GetName(), GetTypeName(Variable->GetType()), Variable->GetDisplayString(), Variable->GetHelp());
			return true;
		}
		// 문자열 변수는 나머지 전체를 값으로 (공백 포함)
		std::string Value = Tokens.front();
		if (Variable->GetType() == EConsoleVariableType::String)
		{
			Value.clear();
			for (const std::string& Token : Tokens)
			{
				Value += (Value.empty() ? "" : " ") + Token;
			}
		}
		std::string Error;
		if (!Variable->Set(Value, EConsoleSetBy::Console, &Error))
		{
			Output.Print(Error);
			return false;
		}
		Output.Printf("{} = {}", Variable->GetName(), Variable->GetDisplayString());
		return true;
	}

	if (const FConsoleCommand* Command = FindCommand(Name))
	{
		if (HasAnyFlags(Command->Flags, EConsoleFlags::Cheat) && !bCheatsAllowed)
		{
			Output.Printf("{}은(는) 치트 명령입니다 (치트가 꺼져 있음)", Command->Name);
			return false;
		}
		if (Command->Execute)
		{
			const FConsoleCommand::FExecuteFn ExecuteFn = Command->Execute; // 명령이 레지스트리를 바꿔도 안전하게 복사
			ExecuteFn(Tokens, Output);
		}
		return true;
	}

	Output.Printf("알 수 없는 명령: {} (help로 목록 보기)", Name);
	return false;
}

std::vector<FConsoleCompletion> FConsoleManager::GetCompletions(std::string_view Input, size_t MaxCount) const
{
	std::vector<FConsoleCompletion> Result;
	const size_t                    First = Input.find_first_not_of(' ');
	const std::string_view          Text  = First == std::string_view::npos ? std::string_view() : Input.substr(First); // 끝 공백은 의미 있음 (값 완성)
	if (Text.empty() || MaxCount == 0)
	{
		return Result;
	}

	// 공백 뒤: 값 이름 / 명령 인자
	if (const size_t Space = Text.find(' '); Space != std::string_view::npos)
	{
		const std::string_view   Name   = Text.substr(0, Space);
		const std::string_view   Rest   = TrimView(Text.substr(Space + 1));
		std::vector<std::string> Candidates;
		std::string              CanonicalName(Name);
		if (const FConsoleVariable* Variable = FindVariable(Name))
		{
			CanonicalName = Variable->GetName();
			if (Variable->GetType() == EConsoleVariableType::Bool)
			{
				Candidates = { "0", "1" };
			}
			else
			{
				Candidates = Variable->GetDesc().ValueNames;
			}
		}
		else if (const FConsoleCommand* Command = FindCommand(Name))
		{
			CanonicalName = Command->Name;
			Candidates    = Command->ArgumentNames;
		}
		for (const std::string& Candidate : Candidates)
		{
			if (Result.size() < MaxCount && ConsoleParsing::StartsWithIgnoreCase(Candidate, Rest))
			{
				Result.push_back({ CanonicalName + " " + Candidate, Candidate, {}, false });
			}
		}
		return Result;
	}

	// 이름: 접두사 일치 → 포함 일치 (각각 이름순)
	struct FEntry
	{
		std::string Name;
		std::string Detail;
		bool        bIsCommand = false;
	};
	std::vector<FEntry> Prefix;
	std::vector<FEntry> Contains;
	const auto          Consider = [&](const std::string& Name, std::string Detail, bool bIsCommand) {
		if (ConsoleParsing::StartsWithIgnoreCase(Name, Text))
		{
			Prefix.push_back({ Name, std::move(Detail), bIsCommand });
		}
		else if (ConsoleParsing::ContainsIgnoreCase(Name, Text))
		{
			Contains.push_back({ Name, std::move(Detail), bIsCommand });
		}
	};
	for (const FConsoleVariable* Variable : GetVariables())
	{
		Consider(Variable->GetName(), std::format("{} — {}", Variable->GetDisplayString(), FirstLine(Variable->GetHelp())), false);
	}
	for (const FConsoleCommand* Command : GetCommands())
	{
		if (FindVariable(Command->Name) == nullptr)
		{
			Consider(Command->Name, FirstLine(Command->Help), true);
		}
	}
	const auto ByName = [](const FEntry& A, const FEntry& B) { return ToKey(A.Name) < ToKey(B.Name); };
	std::sort(Prefix.begin(), Prefix.end(), ByName);
	std::sort(Contains.begin(), Contains.end(), ByName);
	for (std::vector<FEntry>* List : { &Prefix, &Contains })
	{
		for (FEntry& Entry : *List)
		{
			if (Result.size() >= MaxCount)
			{
				return Result;
			}
			Result.push_back({ Entry.Name, Entry.Name, std::move(Entry.Detail), Entry.bIsCommand });
		}
	}
	return Result;
}

std::string FConsoleManager::CompleteInput(std::string_view Input) const
{
	const std::vector<FConsoleCompletion> Completions = GetCompletions(Input, 4096);
	// 접두사 일치 후보만 공통 접두사 계산에 쓴다 (포함 일치는 입력과 앞부분이 다르다)
	std::vector<const std::string*> Matches;
	const std::string_view          Text = TrimView(Input);
	for (const FConsoleCompletion& Completion : Completions)
	{
		if (ConsoleParsing::StartsWithIgnoreCase(Completion.Text, Text))
		{
			Matches.push_back(&Completion.Text);
		}
	}
	if (Matches.empty())
	{
		// 포함 일치만 있으면 첫 후보로
		return Completions.empty() ? std::string(Input) : Completions.front().Text + " ";
	}
	if (Matches.size() == 1)
	{
		return *Matches.front() + " ";
	}
	std::string Common = *Matches.front();
	for (const std::string* Match : Matches)
	{
		size_t Length = 0;
		while (Length < Common.size() && Length < Match->size() && ToLowerAscii(Common[Length]) == ToLowerAscii((*Match)[Length]))
		{
			++Length;
		}
		Common.resize(Length);
	}
	return Common.size() >= Text.size() ? Common : std::string(Input);
}

void FConsoleManager::ApplyCommandLine(const FCommandLine& CommandLine)
{
	FStoredCommandLine Stored;
	const std::vector<std::wstring>& Arguments = CommandLine.GetArguments();
	for (size_t Index = 0; Index < Arguments.size(); ++Index)
	{
		const std::wstring& Argument = Arguments[Index];
		if (Argument.rfind(L"--", 0) != 0)
		{
			continue;
		}
		Stored.Flags.emplace_back(Argument.substr(0, Argument.find(L'=')), std::wstring());
	}
	// 별칭은 FCommandLine 규칙("--key value"/"--key=value")으로 값을 얻는다
	for (auto& [Flag, Value] : Stored.Flags)
	{
		Value = CommandLine.GetValue(Flag);
	}
	for (const std::wstring& List : CommandLine.GetValues(L"--cvar"))
	{
		for (auto& Assignment : ConsoleParsing::ParseAssignmentList(FStringConv::ToUtf8(List)))
		{
			Stored.Assignments.push_back(std::move(Assignment));
		}
	}
	StoredCommandLine = std::move(Stored);

	for (const auto& [Key, Variable] : Variables)
	{
		ApplyCommandLineTo(*Variable);
	}
	for (const auto& [Name, Value] : StoredCommandLine->Assignments)
	{
		if (FindVariable(Name) == nullptr)
		{
			E_LOG(LogConsole, Log, "--cvar {}: 아직 등록되지 않은 변수 (나중에 등록되면 적용)", Name);
		}
	}
}

void FConsoleManager::ApplyCommandLineTo(FConsoleVariable& Variable)
{
	if (!StoredCommandLine)
	{
		return;
	}
	for (const FConsoleCommandLineAlias& Alias : Variable.GetDesc().CommandLine)
	{
		for (const auto& [Flag, Value] : StoredCommandLine->Flags)
		{
			if (Flag != Alias.Flag)
			{
				continue;
			}
			const std::string NewValue = Alias.Value.empty() ? FStringConv::ToUtf8(Value) : Alias.Value;
			std::string       Error;
			if (NewValue.empty() || !Variable.Set(NewValue, EConsoleSetBy::CommandLine, &Error))
			{
				E_LOG(LogConsole, Warning, "{}: {}", FStringConv::ToUtf8(Flag), Error.empty() ? std::string("값이 없습니다") : Error);
			}
			else
			{
				E_LOG(LogConsole, Display, "명령줄 {} → {} = {}", FStringConv::ToUtf8(Flag), Variable.GetName(), Variable.GetDisplayString());
			}
			break;
		}
	}
	for (const auto& [Name, Value] : StoredCommandLine->Assignments)
	{
		if (!ConsoleParsing::EqualsIgnoreCase(Name, Variable.GetName()))
		{
			continue;
		}
		std::string Error;
		if (!Variable.Set(Value, EConsoleSetBy::CommandLine, &Error))
		{
			E_LOG(LogConsole, Warning, "--cvar {}", Error);
		}
		else
		{
			E_LOG(LogConsole, Display, "--cvar {} = {}", Variable.GetName(), Variable.GetDisplayString());
		}
	}
}

void FConsoleManager::AddHistory(std::string_view Line)
{
	const std::string_view Trimmed = TrimView(Line);
	if (Trimmed.empty())
	{
		return;
	}
	History.erase(std::remove(History.begin(), History.end(), Trimmed), History.end()); // 같은 줄은 맨 뒤로
	History.emplace_back(Trimmed);
	if (History.size() > MaxHistory)
	{
		History.erase(History.begin(), History.begin() + static_cast<std::ptrdiff_t>(History.size() - MaxHistory));
	}
}

// ---------------------------------------------------------------- 기본 명령

void FConsoleManager::RegisterBuiltins()
{
	RegisterVariable("stat.FPS", EConsoleVariableType::Bool, "0", "화면 통계: 프레임 시간/FPS (stat fps로 켜고 끔)");
	RegisterVariable("stat.GPU", EConsoleVariableType::Bool, "0", "화면 통계: 렌더 구간별 CPU/GPU 시간 (stat gpu로 켜고 끔)");
	RegisterVariable("stat.Memory", EConsoleVariableType::Bool, "0", "화면 통계: VRAM 사용량/예산과 리소스 수·바이트 (stat memory로 켜고 끔)");

	RegisterCommand({ "help", "명령/변수 목록 (help <이름>: 그 항목 설명)",
	                  [this](const std::vector<std::string>& Args, const FConsoleOutput& Output) {
		                  if (!Args.empty())
		                  {
			                  if (const FConsoleVariable* Variable = FindVariable(Args.front()))
			                  {
				                  Output.Printf("{} ({}) = {} — {}", Variable->GetName(), GetTypeName(Variable->GetType()), Variable->GetDisplayString(),
				                                Variable->GetHelp());
				                  if (!Variable->GetDesc().ValueNames.empty())
				                  {
					                  std::string Names;
					                  for (size_t Index = 0; Index < Variable->GetDesc().ValueNames.size(); ++Index)
					                  {
						                  Names += std::format("{}{}={}", Names.empty() ? "" : ", ", Index, Variable->GetDesc().ValueNames[Index]);
					                  }
					                  Output.Printf("  값: {}", Names);
				                  }
				                  return;
			                  }
			                  if (const FConsoleCommand* Command = FindCommand(Args.front()))
			                  {
				                  Output.Printf("{} — {}", Command->Name, Command->Help);
				                  return;
			                  }
			                  Output.Printf("알 수 없는 이름: {}", Args.front());
			                  return;
		                  }
		                  Output.Print("명령:");
		                  for (const FConsoleCommand* Command : GetCommands())
		                  {
			                  Output.Printf("  {} — {}", Command->Name, Command->Help);
		                  }
		                  Output.Printf("변수 {}개 (cvars [필터]로 보기). 변수: \"이름\" 값 보기, \"이름 값\" 설정, \"이름 ?\" 설명", Variables.size());
	                  } });

	RegisterCommand({ "cvars", "콘솔 변수 목록과 현재 값 (cvars [필터]: 이름에 필터가 들어간 것만)",
	                  [this](const std::vector<std::string>& Args, const FConsoleOutput& Output) {
		                  const std::string Filter = Args.empty() ? std::string() : Args.front();
		                  size_t            Count  = 0;
		                  for (const FConsoleVariable* Variable : GetVariables())
		                  {
			                  if (!ConsoleParsing::ContainsIgnoreCase(Variable->GetName(), Filter))
			                  {
				                  continue;
			                  }
			                  Output.Printf("  {} = {}{} — {}", Variable->GetName(), Variable->GetDisplayString(), Variable->IsDefault() ? "" : " *",
			                                FirstLine(Variable->GetHelp()));
			                  ++Count;
		                  }
		                  Output.Printf("{}개 (* = 기본값과 다름)", Count);
	                  } });

	RegisterCommand({ "stat", "화면 통계 켜고 끄기: stat fps | stat gpu | stat memory | stat none",
	                  [this](const std::vector<std::string>& Args, const FConsoleOutput& Output) {
		                  FConsoleVariable* Fps    = FindVariable("stat.FPS");
		                  FConsoleVariable* Gpu    = FindVariable("stat.GPU");
		                  FConsoleVariable* Memory = FindVariable("stat.Memory");
		                  if (Args.empty() || Fps == nullptr || Gpu == nullptr || Memory == nullptr)
		                  {
			                  Output.Print("사용: stat fps | stat gpu | stat memory | stat none");
			                  return;
		                  }
		                  const std::string& Mode = Args.front();
		                  if (ConsoleParsing::EqualsIgnoreCase(Mode, "fps"))
		                  {
			                  Fps->SetBool(!Fps->GetBool());
		                  }
		                  else if (ConsoleParsing::EqualsIgnoreCase(Mode, "gpu"))
		                  {
			                  Gpu->SetBool(!Gpu->GetBool());
		                  }
		                  else if (ConsoleParsing::EqualsIgnoreCase(Mode, "memory"))
		                  {
			                  Memory->SetBool(!Memory->GetBool());
		                  }
		                  else if (ConsoleParsing::EqualsIgnoreCase(Mode, "none"))
		                  {
			                  Fps->SetBool(false);
			                  Gpu->SetBool(false);
			                  Memory->SetBool(false);
		                  }
		                  else
		                  {
			                  Output.Printf("알 수 없는 통계: {} (fps | gpu | memory | none)", Mode);
			                  return;
		                  }
		                  Output.Printf("화면 통계: fps {}, gpu {}, memory {}", Fps->GetBool() ? "켬" : "끔", Gpu->GetBool() ? "켬" : "끔",
		                                Memory->GetBool() ? "켬" : "끔");
	                  },
	                  { "fps", "gpu", "memory", "none" } });
}

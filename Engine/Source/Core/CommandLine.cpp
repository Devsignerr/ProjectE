#include "Core/CommandLine.h"

#include "Core/Platform/WindowsHeaders.h"

#include <shellapi.h>

namespace
{
	// CommandLineToArgvW는 첫 토큰을 프로그램 이름으로 특별 취급하므로 항상 더미를 앞에 붙여 파싱한다
	std::vector<std::wstring> SplitArguments(std::wstring_view Arguments)
	{
		std::vector<std::wstring> Result;

		const std::wstring FullLine = L"dummy.exe " + std::wstring(Arguments);
		int                Count    = 0;
		LPWSTR*            Argv     = CommandLineToArgvW(FullLine.c_str(), &Count);
		if (Argv == nullptr)
		{
			return Result;
		}

		Result.reserve(static_cast<size_t>(Count > 0 ? Count - 1 : 0));
		for (int Index = 1; Index < Count; ++Index)
		{
			Result.emplace_back(Argv[Index]);
		}
		LocalFree(Argv);
		return Result;
	}

	// "--key=value" 형태에서 key 부분과 value 부분 분리. '='가 없으면 value는 비어 있다.
	void SplitKeyValue(std::wstring_view Argument, std::wstring_view& OutKey, std::wstring_view& OutValue)
	{
		const size_t Equals = Argument.find(L'=');
		if (Equals == std::wstring_view::npos)
		{
			OutKey   = Argument;
			OutValue = {};
		}
		else
		{
			OutKey   = Argument.substr(0, Equals);
			OutValue = Argument.substr(Equals + 1);
		}
	}
} // namespace

FCommandLine FCommandLine::FromProcess()
{
	FCommandLine Result;

	int     Count = 0;
	LPWSTR* Argv  = CommandLineToArgvW(GetCommandLineW(), &Count);
	if (Argv == nullptr)
	{
		return Result;
	}

	Result.Arguments.reserve(static_cast<size_t>(Count > 0 ? Count - 1 : 0));
	for (int Index = 1; Index < Count; ++Index)
	{
		Result.Arguments.emplace_back(Argv[Index]);
	}
	LocalFree(Argv);
	return Result;
}

FCommandLine FCommandLine::Parse(std::wstring_view InArguments)
{
	FCommandLine Result;
	Result.Arguments = SplitArguments(InArguments);
	return Result;
}

bool FCommandLine::HasFlag(std::wstring_view Flag) const
{
	for (const std::wstring& Argument : Arguments)
	{
		std::wstring_view Key, Value;
		SplitKeyValue(Argument, Key, Value);
		if (Key == Flag)
		{
			return true;
		}
	}
	return false;
}

std::wstring FCommandLine::GetValue(std::wstring_view InKey) const
{
	for (size_t Index = 0; Index < Arguments.size(); ++Index)
	{
		std::wstring_view Key, Value;
		SplitKeyValue(Arguments[Index], Key, Value);
		if (Key != InKey)
		{
			continue;
		}
		if (!Value.empty())
		{
			return std::wstring(Value);
		}
		// "--key value": 다음 인자가 또 다른 옵션("--")이 아니면 값으로 취급
		if (Index + 1 < Arguments.size() && Arguments[Index + 1].rfind(L"--", 0) != 0)
		{
			return Arguments[Index + 1];
		}
		return {};
	}
	return {};
}

std::vector<std::wstring> FCommandLine::GetValues(std::wstring_view InKey) const
{
	std::vector<std::wstring> Values;
	for (size_t Index = 0; Index < Arguments.size(); ++Index)
	{
		std::wstring_view Key, Value;
		SplitKeyValue(Arguments[Index], Key, Value);
		if (Key != InKey)
		{
			continue;
		}
		if (!Value.empty())
		{
			Values.emplace_back(Value);
		}
		else if (Index + 1 < Arguments.size() && Arguments[Index + 1].rfind(L"--", 0) != 0)
		{
			Values.push_back(Arguments[++Index]);
		}
	}
	return Values;
}

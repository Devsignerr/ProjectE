#include "Core/Log.h"

#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

E_DEFINE_LOG_CATEGORY(LogCore, Log)

namespace
{
	std::mutex GLogMutex;
	bool       GbConsoleColorEnabled = false;

	const char* ToString(ELogVerbosity Verbosity)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Fatal:   return "Fatal";
		case ELogVerbosity::Error:   return "Error";
		case ELogVerbosity::Warning: return "Warning";
		case ELogVerbosity::Display: return "Display";
		case ELogVerbosity::Log:     return "Log";
		case ELogVerbosity::Verbose: return "Verbose";
		}
		return "Unknown";
	}

	// ANSI 색상 코드 (가상 터미널 처리 활성화 시)
	const char* ToConsoleColor(ELogVerbosity Verbosity)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Fatal:
		case ELogVerbosity::Error:   return "\x1b[91m";
		case ELogVerbosity::Warning: return "\x1b[93m";
		case ELogVerbosity::Display: return "\x1b[92m";
		case ELogVerbosity::Verbose: return "\x1b[90m";
		default:                     return "\x1b[0m";
		}
	}

	void EnableConsoleVirtualTerminal()
	{
		HANDLE Console = GetStdHandle(STD_OUTPUT_HANDLE);
		if (Console == nullptr || Console == INVALID_HANDLE_VALUE)
		{
			return;
		}

		DWORD Mode = 0;
		if (!GetConsoleMode(Console, &Mode))
		{
			return;
		}

		GbConsoleColorEnabled = SetConsoleMode(Console, Mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
	}
} // namespace

void FLog::Init()
{
	SetConsoleOutputCP(CP_UTF8);
	EnableConsoleVirtualTerminal();
}

void FLog::Shutdown()
{
	std::fflush(stdout);
}

bool FLog::ShouldLog(const FLogCategory& Category, ELogVerbosity Verbosity)
{
	return Verbosity <= Category.MaxVerbosity;
}

void FLog::Write(const FLogCategory& Category, ELogVerbosity Verbosity, std::string_view Message)
{
	SYSTEMTIME Time;
	GetLocalTime(&Time);

	const std::string Line = std::format("[{:02}:{:02}:{:02}.{:03}] {}: {}: {}\n",
	                                     Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds,
	                                     Category.Name, ToString(Verbosity), Message);

	{
		std::scoped_lock Lock(GLogMutex);

		if (GbConsoleColorEnabled)
		{
			std::fputs(ToConsoleColor(Verbosity), stdout);
			std::fputs(Line.c_str(), stdout);
			std::fputs("\x1b[0m", stdout);
		}
		else
		{
			std::fputs(Line.c_str(), stdout);
		}

		OutputDebugStringW(FStringConv::ToWide(Line).c_str());
	}

	if (Verbosity == ELogVerbosity::Fatal)
	{
		std::fflush(stdout);
		if (IsDebuggerPresent())
		{
			__debugbreak();
		}
		std::abort();
	}
}

#include "Core/Log.h"

#include "Core/Platform/CrashHandler.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>

E_DEFINE_LOG_CATEGORY(LogCore, Log)

namespace
{
	std::mutex    GLogMutex;
	std::ofstream GLogFile; // SetFileOutput
	std::filesystem::path GLogFilePath;
	bool       GbConsoleColorEnabled = false;
	bool       GbHistoryEnabled = false;
	uint64     GNextLogSequence = 1;
	std::deque<FLogMessage> GLogHistory;

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

void FLog::EnableHistory()
{
	std::scoped_lock Lock(GLogMutex);
	GbHistoryEnabled = true;
}

std::vector<FLogMessage> FLog::ReadHistory(uint64 AfterSequence)
{
	std::scoped_lock Lock(GLogMutex);
	std::vector<FLogMessage> Messages;
	if (GLogHistory.empty() || GLogHistory.back().Sequence <= AfterSequence)
	{
		return Messages;
	}
	for (const FLogMessage& Message : GLogHistory)
	{
		if (Message.Sequence > AfterSequence)
		{
			Messages.push_back(Message);
		}
	}
	return Messages;
}

bool FLog::ShouldLog(const FLogCategory& Category, ELogVerbosity Verbosity)
{
	return Verbosity <= Category.MaxVerbosity;
}

bool FLog::SetFileOutput(const std::filesystem::path& Path)
{
	std::scoped_lock Lock(GLogMutex);
	GLogFile.close();
	GLogFilePath.clear();
	if (Path.empty())
	{
		return true;
	}
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	GLogFile.open(Path, std::ios::binary | std::ios::trunc);
	if (GLogFile.is_open())
	{
		GLogFilePath = Path;
	}
	return GLogFile.is_open();
}

std::filesystem::path FLog::GetFileOutputPath()
{
	return GLogFilePath; // 크래시 경로에서도 불리므로 잠그지 않는다 (SetFileOutput은 시작 때만)
}

void FLog::WriteEmergency(std::string_view Text)
{
	const std::string Copy(Text);
	std::fflush(stdout);
	std::fputs(Copy.c_str(), stderr);
	std::fflush(stderr);
	OutputDebugStringA(Copy.c_str());
	if (GLogFile.is_open())
	{
		GLogFile << Copy;
		GLogFile.flush();
	}
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
		if (GbHistoryEnabled)
		{
			GLogHistory.push_back({ GNextLogSequence++, Verbosity, Line });
			if (GLogHistory.size() > MaxHistoryMessages)
			{
				GLogHistory.pop_front();
			}
		}

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
		if (GLogFile.is_open())
		{
			GLogFile << Line;
			GLogFile.flush(); // 비정상 종료에도 마지막 줄까지 남도록
		}
	}

	if (Verbosity == ELogVerbosity::Fatal)
	{
		std::fflush(stdout);
		if (IsDebuggerPresent())
		{
			__debugbreak();
		}
		FCrashHandler::ReportFatal(Line); // 덤프 폴더가 지정되어 있으면 미니덤프
		std::abort();
	}
}

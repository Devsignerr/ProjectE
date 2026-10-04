#include "Core/Platform/CrashHandler.h"

#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <DbgHelp.h>
#include <crtdbg.h>

#include <cstdio>
#include <format>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace
{
	// Fatal 로그용 사용자 예외 코드 (소프트웨어 예외 영역 0xE...)
	constexpr DWORD GFatalExceptionCode = 0xE0F00001;
	// ntdll 힙 관리자가 손상을 발견하면 이 코드를 첫 단계 예외로 올린 뒤 프로세스를 바로 끝낸다(빠른 실패) —
	// 처리되지 않은 예외 필터까지 가지 않으므로 벡터 예외 처리기로 잡는다
	constexpr DWORD GHeapCorruptionCode     = 0xC0000374; // STATUS_HEAP_CORRUPTION
	constexpr DWORD GStackBufferOverrunCode = 0xC0000409; // STATUS_STACK_BUFFER_OVERRUN (__fastfail)

	std::filesystem::path GDumpDirectory;
	bool                  GbShowDialog = false;
	volatile LONG         GbHandlingCrash = 0; // 크래시 처리 중 다시 크래시하면 건너뛴다

	const char* DescribeException(DWORD Code)
	{
		switch (Code)
		{
		case EXCEPTION_ACCESS_VIOLATION:      return "액세스 위반";
		case EXCEPTION_STACK_OVERFLOW:        return "스택 오버플로";
		case EXCEPTION_ILLEGAL_INSTRUCTION:   return "잘못된 명령";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "정수 0 나누기";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "배열 범위 초과";
		case EXCEPTION_BREAKPOINT:            return "중단점";
		case GFatalExceptionCode:             return "Fatal 로그";
		case GHeapCorruptionCode:             return "힙 손상";
		case GStackBufferOverrunCode:         return "스택 버퍼 오버런/빠른 실패";
		default:                              return "알 수 없는 예외";
		}
	}

	// 콜스택을 문자열로 (StackWalk64 + 심볼/소스 줄)
	std::string BuildCallStack(const CONTEXT& InContext)
	{
		const HANDLE Process = GetCurrentProcess();
		const HANDLE Thread  = GetCurrentThread();
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(Process, nullptr, TRUE);

		CONTEXT      Context = InContext;
		STACKFRAME64 Frame{};
		Frame.AddrPC.Offset    = Context.Rip;
		Frame.AddrPC.Mode      = AddrModeFlat;
		Frame.AddrFrame.Offset = Context.Rbp;
		Frame.AddrFrame.Mode   = AddrModeFlat;
		Frame.AddrStack.Offset = Context.Rsp;
		Frame.AddrStack.Mode   = AddrModeFlat;

		alignas(SYMBOL_INFO) char SymbolBuffer[sizeof(SYMBOL_INFO) + 512]{};
		SYMBOL_INFO* Symbol  = reinterpret_cast<SYMBOL_INFO*>(SymbolBuffer);
		Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		Symbol->MaxNameLen   = 511;

		std::string Stack;
		for (int Depth = 0; Depth < 48; ++Depth)
		{
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, Process, Thread, &Frame, &Context, nullptr,
			                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || Frame.AddrPC.Offset == 0)
			{
				break;
			}

			const DWORD64 Address = Frame.AddrPC.Offset;
			DWORD64       Displacement = 0;
			const char*   Name = SymFromAddr(Process, Address, &Displacement, Symbol) ? Symbol->Name : "?";

			IMAGEHLP_LINE64 Line{};
			Line.SizeOfStruct = sizeof(Line);
			DWORD LineDisplacement = 0;
			if (SymGetLineFromAddr64(Process, Address, &LineDisplacement, &Line))
			{
				Stack += std::format("  #{:02} {} ({}:{})\n", Depth, Name, Line.FileName, Line.LineNumber);
			}
			else
			{
				Stack += std::format("  #{:02} {} (0x{:X})\n", Depth, Name, Address);
			}
		}
		SymCleanup(Process);
		return Stack;
	}

	// <덤프 폴더>/<시각>/에 미니덤프 + 보고서 + 로그 사본. 실패하면 빈 경로
	std::filesystem::path WriteCrashFiles(EXCEPTION_POINTERS* Info, const std::string& Report)
	{
		if (GDumpDirectory.empty())
		{
			return {};
		}
		SYSTEMTIME Time;
		GetLocalTime(&Time);
		const std::filesystem::path Directory = GDumpDirectory / std::format(L"{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
		                                                                      Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);
		std::error_code ErrorCode;
		std::filesystem::create_directories(Directory, ErrorCode);
		if (ErrorCode)
		{
			return {};
		}

		const HANDLE DumpFile = CreateFileW((Directory / L"Minidump.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (DumpFile != INVALID_HANDLE_VALUE)
		{
			MINIDUMP_EXCEPTION_INFORMATION ExceptionInfo{};
			ExceptionInfo.ThreadId          = GetCurrentThreadId();
			ExceptionInfo.ExceptionPointers = Info;
			ExceptionInfo.ClientPointers    = FALSE;
			// 스택·스레드 정보 + 스택이 가리키는 메모리 (수 MB 이내, 전체 힙은 넣지 않는다)
			const auto DumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), DumpFile, DumpType, Info != nullptr ? &ExceptionInfo : nullptr, nullptr, nullptr);
			CloseHandle(DumpFile);
		}

		const HANDLE ReportFile = CreateFileW((Directory / L"CrashReport.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (ReportFile != INVALID_HANDLE_VALUE)
		{
			DWORD Written = 0;
			WriteFile(ReportFile, Report.data(), static_cast<DWORD>(Report.size()), &Written, nullptr);
			CloseHandle(ReportFile);
		}
		// 로그는 줄마다 flush되므로 파일 복사로 충분하다
		if (const std::filesystem::path LogPath = FLog::GetFileOutputPath(); !LogPath.empty())
		{
			CopyFileW(LogPath.c_str(), (Directory / LogPath.filename()).c_str(), FALSE);
		}
		return Directory;
	}

	void ShowCrashDialog(const std::filesystem::path& CrashDirectory)
	{
		if (!GbShowDialog)
		{
			return;
		}
		std::wstring Text = L"게임이 예기치 않게 종료되었습니다.";
		if (!CrashDirectory.empty())
		{
			Text += L"\n\n오류 보고서가 저장되었습니다:\n" + CrashDirectory.wstring();
		}
		MessageBoxW(nullptr, Text.c_str(), L"오류", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
	}

	std::string BuildReport(EXCEPTION_POINTERS* Info)
	{
		const EXCEPTION_RECORD& Record = *Info->ExceptionRecord;
		std::string Report = std::format("\n==== 크래시: {} (0x{:08X}) 주소 {} ====\n",
		                                 DescribeException(Record.ExceptionCode), Record.ExceptionCode, Record.ExceptionAddress);
		if (Record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && Record.NumberParameters >= 2)
		{
			Report += std::format("{} 대상 주소 0x{:X}\n", Record.ExceptionInformation[0] == 1 ? "쓰기" : "읽기",
			                      Record.ExceptionInformation[1]);
		}
		// 스택 오버플로 시에는 남은 스택이 적어 심볼화를 생략한다
		if (Record.ExceptionCode != EXCEPTION_STACK_OVERFLOW)
		{
			Report += BuildCallStack(*Info->ContextRecord);
		}
		return Report;
	}

	LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* Info)
	{
		if (IsDebuggerPresent())
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}
		if (InterlockedExchange(&GbHandlingCrash, 1) != 0)
		{
			return EXCEPTION_EXECUTE_HANDLER;
		}

		const std::string Report = BuildReport(Info);
		FLog::WriteEmergency(Report);
		const std::filesystem::path CrashDirectory = WriteCrashFiles(Info, Report);
		if (!CrashDirectory.empty())
		{
			FLog::WriteEmergency(std::format("크래시 보고서: {}\n", FStringConv::ToUtf8(CrashDirectory.wstring())));
		}
		ShowCrashDialog(CrashDirectory);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// Fatal: 사용자 예외를 던져 현재 컨텍스트로 덤프를 쓴다 (__try가 있는 함수에는 소멸자가 있는 객체를 둘 수 없어 분리)
	LONG WriteFatalDump(EXCEPTION_POINTERS* Info, const char* Message)
	{
		if (InterlockedExchange(&GbHandlingCrash, 1) == 0)
		{
			const std::string Report = std::format("\n==== Fatal: {} ====\n{}", Message, BuildCallStack(*Info->ContextRecord));
			const std::filesystem::path CrashDirectory = WriteCrashFiles(Info, Report);
			ShowCrashDialog(CrashDirectory);
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}

	void RaiseFatal(const char* Message)
	{
		__try
		{
			RaiseException(GFatalExceptionCode, 0, 0, nullptr);
		}
		__except (WriteFatalDump(GetExceptionInformation(), Message))
		{
		}
	}

	// 힙 손상·빠른 실패는 처리되지 않은 예외 필터를 거치지 않으므로 첫 단계에서 보고서를 남긴다 (발견 지점 콜스택 — 손상을 낸 곳은 아닐 수 있다)
	LONG WINAPI HandleVectoredException(EXCEPTION_POINTERS* Info)
	{
		const DWORD Code = Info->ExceptionRecord->ExceptionCode;
		if ((Code == GHeapCorruptionCode || Code == GStackBufferOverrunCode) && !IsDebuggerPresent())
		{
			HandleUnhandledException(Info);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	bool GbUnattended = false;

#if defined(_DEBUG)
	// 디버그 CRT 보고(assert, 힙 검사 실패 등): 내용을 로그에 남기고, 사람이 없는 실행(자동 검증)이면 대화 상자 대신 덤프 후 종료한다
	// (숨긴 창 자동 검증에서 CRT 대화 상자가 뜨면 보이지 않은 채 시간 초과로 끝나 원인을 잃는다)
	int __cdecl HandleCrtReport(int ReportType, char* Message, int* ReturnValue)
	{
		if (ReportType == _CRT_WARN)
		{
			return FALSE;
		}
		const std::string Text = std::format("\n==== CRT {}: {} ====\n", ReportType == _CRT_ASSERT ? "assert" : "오류", Message != nullptr ? Message : "");
		FLog::WriteEmergency(Text);
		if (!GbUnattended)
		{
			return FALSE; // 기본 처리(대화 상자)
		}
		RaiseFatal(Text.c_str());
		if (ReturnValue != nullptr)
		{
			*ReturnValue = 0;
		}
		TerminateProcess(GetCurrentProcess(), 3);
		return TRUE;
	}
#endif
}

void FCrashHandler::Install()
{
	static bool bInstalled = false;
	if (!bInstalled)
	{
		bInstalled = true;
		SetUnhandledExceptionFilter(&HandleUnhandledException);
		AddVectoredExceptionHandler(0, &HandleVectoredException);
#if defined(_DEBUG)
		_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &HandleCrtReport);
#endif
	}
}

void FCrashHandler::SetUnattended(bool bUnattended)
{
	GbUnattended = bUnattended;
}

void FCrashHandler::SetDumpDirectory(const std::filesystem::path& Directory)
{
	GDumpDirectory = Directory;
}

void FCrashHandler::SetShowDialog(bool bShow)
{
	GbShowDialog = bShow;
}

void FCrashHandler::ReportFatal(std::string_view Message)
{
	if (IsDebuggerPresent() || GDumpDirectory.empty())
	{
		return;
	}
	const std::string Copy(Message);
	RaiseFatal(Copy.c_str());
}

#include "Core/Platform/CrashHandler.h"

#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"

#include <DbgHelp.h>

#include <cstdio>
#include <format>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace
{
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

	LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* Info)
	{
		if (IsDebuggerPresent())
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

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

		FLog::WriteEmergency(Report);
		return EXCEPTION_EXECUTE_HANDLER;
	}
}

void FCrashHandler::Install()
{
	static bool bInstalled = false;
	if (!bInstalled)
	{
		bInstalled = true;
		SetUnhandledExceptionFilter(&HandleUnhandledException);
	}
}

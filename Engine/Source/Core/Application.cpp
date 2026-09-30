#include "Core/Application.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/CrashHandler.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <chrono>
#include <string>

FApplication::FApplication(const FApplicationDesc& InDesc)
	: Desc(InDesc)
{
	// 파생 클래스 멤버 생성보다 먼저 설치되어 초기화 중 크래시도 잡는다
	FCrashHandler::Install();
}

int FApplication::Run()
{
	FLog::Init();
	if (!FPaths::IsInitialized())
	{
		FPaths::Initialize();
	}

	// 자동 검증 인자
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	if (const std::wstring LogPath = CommandLine.GetValue(L"--log"); !LogPath.empty())
	{
		FLog::SetFileOutput(LogPath);
	}
	ScreenshotPath = CommandLine.GetValue(L"--screenshot");
	if (const std::wstring ExitAfter = CommandLine.GetValue(L"--exit-after"); !ExitAfter.empty())
	{
		ExitAfterFrames = static_cast<uint64>(std::max(1LL, std::stoll(ExitAfter)));
	}
	else if (!ScreenshotPath.empty())
	{
		ExitAfterFrames = 90; // 셰이더/에셋 로드와 자동 노출이 안정될 시간
	}
	if (Desc.bHeadless)
	{
		ScreenshotPath.clear(); // 렌더가 없으므로 무시
	}
	if (ExitAfterFrames > 0)
	{
		E_LOG(LogCore, Display, "자동 검증 모드: {} {} 후 종료{}", ExitAfterFrames, Desc.bHeadless ? "틱" : "프레임",
		      ScreenshotPath.empty() ? "" : ", 스크린샷 " + FStringConv::ToUtf8(ScreenshotPath.wstring()));
	}

	if (!Desc.bHeadless)
	{
		// 모니터별 DPI 인식 (창 크기/좌표가 논리 픽셀로 스케일되지 않도록)
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

		Window.SetEventHandler([this](const FWindowEvent& Event) { HandleWindowEvent(Event); });
		if (!Window.Create(Desc.Window))
		{
			E_LOG(LogCore, Error, "창 생성에 실패하여 종료합니다");
			return -1;
		}
	}

	if (!OnInit())
	{
		E_LOG(LogCore, Error, "애플리케이션 초기화에 실패하여 종료합니다");
		Window.Destroy();
		FLog::Shutdown();
		return -1;
	}

	E_LOG(LogCore, Display, "메인 루프 시작{}", Desc.bHeadless ? " (헤드리스)" : "");
	Timer.Reset();
	if (Desc.bHeadless)
	{
		RunHeadlessLoop();
	}
	else
	{
		RunWindowedLoop();
	}

	E_LOG(LogCore, Display, "종료 중...");
	OnShutdown();
	Window.Destroy();
	FLog::Shutdown();
	return 0;
}

void FApplication::RunWindowedLoop()
{
	while (!bExitRequested)
	{
		Window.PumpMessages();
		if (bExitRequested)
		{
			break;
		}

		Timer.Tick();
		OnUpdate(Timer.GetDeltaSeconds());

		if (Window.IsMinimized())
		{
			// 최소화 상태에서는 렌더링을 건너뛰고 CPU 점유를 낮춘다
			Sleep(10);
		}
		else
		{
			if (!ScreenshotPath.empty() && ExitAfterFrames > 0 && FrameIndex + 1 == ExitAfterFrames)
			{
				OnScreenshotRequested(ScreenshotPath);
			}
			OnRender();
			++FrameIndex;
			if (ExitAfterFrames > 0 && FrameIndex >= ExitAfterFrames)
			{
				RequestExit();
			}
		}

		Input.EndFrame();
	}
}

namespace
{
	// 콘솔 Ctrl+C/창 닫기 → 종료 요청 (처리기는 별도 스레드에서 불린다)
	FApplication* GHeadlessApp = nullptr;

	BOOL WINAPI HandleConsoleControl(DWORD ControlType)
	{
		if (GHeadlessApp != nullptr && (ControlType == CTRL_C_EVENT || ControlType == CTRL_BREAK_EVENT || ControlType == CTRL_CLOSE_EVENT))
		{
			GHeadlessApp->RequestExit();
			return TRUE;
		}
		return FALSE;
	}
} // namespace

void FApplication::RunHeadlessLoop()
{
	GHeadlessApp = this;
	SetConsoleCtrlHandler(&HandleConsoleControl, TRUE);

	// 고해상도 대기 타이머 (Sleep은 기본 15.6ms 단위라 60Hz 틱이 흔들린다)
	HANDLE    WaitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	using FClock = std::chrono::steady_clock;
	const auto TickDuration = std::chrono::duration_cast<FClock::duration>(std::chrono::duration<double>(std::max(Desc.HeadlessTickSeconds, 0.001f)));
	auto       NextTick     = FClock::now();

	while (!bExitRequested)
	{
		Timer.Tick();
		OnUpdate(Timer.GetDeltaSeconds());
		++FrameIndex;
		if (ExitAfterFrames > 0 && FrameIndex >= ExitAfterFrames)
		{
			RequestExit();
		}

		// 다음 틱까지 대기. 많이 밀렸으면 따라잡지 않고 기준을 현재로 옮긴다
		NextTick += TickDuration;
		const auto Now = FClock::now();
		if (NextTick < Now - TickDuration)
		{
			NextTick = Now;
		}
		else if (NextTick > Now)
		{
			const auto Remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(NextTick - Now).count();
			if (WaitTimer != nullptr)
			{
				LARGE_INTEGER DueTime;
				DueTime.QuadPart = -static_cast<LONGLONG>(Remaining / 100); // 100ns 단위, 음수 = 상대 시간
				SetWaitableTimer(WaitTimer, &DueTime, 0, nullptr, nullptr, FALSE);
				WaitForSingleObject(WaitTimer, INFINITE);
			}
			else
			{
				Sleep(static_cast<DWORD>(Remaining / 1000000));
			}
		}
	}

	if (WaitTimer != nullptr)
	{
		CloseHandle(WaitTimer);
	}
	SetConsoleCtrlHandler(&HandleConsoleControl, FALSE);
	GHeadlessApp = nullptr;
}

void FApplication::HandleWindowEvent(const FWindowEvent& Event)
{
	Input.ProcessEvent(Event);

	switch (Event.Type)
	{
	case EWindowEventType::Close:
		RequestExit();
		break;

	case EWindowEventType::Resize:
		if (!Event.bMinimized && Event.Width > 0 && Event.Height > 0)
		{
			OnResize(Event.Width, Event.Height);
		}
		break;

	default:
		break;
	}
}

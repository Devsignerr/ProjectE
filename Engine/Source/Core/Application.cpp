#include "Core/Application.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <string>

FApplication::FApplication(const FApplicationDesc& InDesc)
	: Desc(InDesc)
{
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
	if (ExitAfterFrames > 0)
	{
		E_LOG(LogCore, Display, "자동 검증 모드: {} 프레임 후 종료{}", ExitAfterFrames,
		      ScreenshotPath.empty() ? "" : ", 스크린샷 " + FStringConv::ToUtf8(ScreenshotPath.wstring()));
	}

	// 모니터별 DPI 인식 (창 크기/좌표가 논리 픽셀로 스케일되지 않도록)
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	Window.SetEventHandler([this](const FWindowEvent& Event) { HandleWindowEvent(Event); });
	if (!Window.Create(Desc.Window))
	{
		E_LOG(LogCore, Error, "창 생성에 실패하여 종료합니다");
		return -1;
	}

	if (!OnInit())
	{
		E_LOG(LogCore, Error, "애플리케이션 초기화에 실패하여 종료합니다");
		Window.Destroy();
		FLog::Shutdown();
		return -1;
	}

	E_LOG(LogCore, Display, "메인 루프 시작");
	Timer.Reset();

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

	E_LOG(LogCore, Display, "종료 중...");
	OnShutdown();
	Window.Destroy();
	FLog::Shutdown();
	return 0;
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

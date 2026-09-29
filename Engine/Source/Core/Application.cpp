#include "Core/Application.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"

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
			OnRender();
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

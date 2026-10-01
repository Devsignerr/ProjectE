#include "Core/Application.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/CrashHandler.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>

namespace
{
	// 기본 로그 파일 <Saved>/Logs/<실행 파일 이름>.log. 이전 실행 로그는 -backup으로 하나 보관한다.
	// 같은 앱이 이미 돌고 있어 이전 파일을 옮길 수 없으면(쓰는 중) 프로세스 번호를 붙인 파일을 쓴다
	std::filesystem::path OpenDefaultLogFile()
	{
		wchar_t     Buffer[MAX_PATH * 4];
		const DWORD Length = GetModuleFileNameW(nullptr, Buffer, static_cast<DWORD>(std::size(Buffer)));
		const std::wstring Stem = std::filesystem::path(Buffer, Buffer + Length).stem().wstring();

		const std::filesystem::path Directory = FPaths::GetLogDirectory();
		std::filesystem::path       LogPath   = Directory / (Stem + L".log");
		std::error_code             ErrorCode;
		if (std::filesystem::exists(LogPath, ErrorCode))
		{
			const std::filesystem::path BackupPath = Directory / (Stem + L"-backup.log");
			std::filesystem::remove(BackupPath, ErrorCode);
			std::filesystem::rename(LogPath, BackupPath, ErrorCode);
			if (ErrorCode)
			{
				LogPath = Directory / std::format(L"{}-{}.log", Stem, GetCurrentProcessId());
			}
		}
		return FLog::SetFileOutput(LogPath) ? LogPath : std::filesystem::path();
	}
} // namespace

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
	else if (const std::filesystem::path DefaultLog = OpenDefaultLogFile(); !DefaultLog.empty())
	{
		E_LOG(LogCore, Log, "로그 파일: {}", FStringConv::ToUtf8(DefaultLog.wstring()));
	}
	// FPaths 초기화 로그는 파일을 열기 전이므로 요약을 다시 남긴다 (배포 환경 진단용)
	E_LOG(LogCore, Display, "엔진: {}{}, 프로젝트: {}, pak 파일 {}개", FStringConv::ToUtf8(FPaths::GetEngineDirectory().wstring()), FPaths::IsPackaged() ? " (패키지)" : "",
	      FPaths::HasProject() ? FStringConv::ToUtf8(FPaths::GetProjectFile().wstring()) : std::string("없음"), FFileSystem::GetMountedFileCount());
	FCrashHandler::SetDumpDirectory(FPaths::GetCrashDirectory());
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
	// 크래시 알림 대화 상자는 패키지 게임 창에서만 (개발/자동 검증은 로그와 덤프로 충분, 서버는 사람이 없다)
	FCrashHandler::SetShowDialog(FPaths::IsPackaged() && !Desc.bHeadless && ExitAfterFrames == 0);
	// 쉼표 목록 "W,Space" / "A,LeftY=1"
	const auto SplitList = [](const std::wstring& List) {
		std::vector<std::string> Items;
		size_t                   Begin = 0;
		while (Begin <= List.size())
		{
			const size_t End = std::min(List.find(L',', Begin), List.size());
			if (End > Begin)
			{
				Items.push_back(FStringConv::ToUtf8(List.substr(Begin, End - Begin)));
			}
			Begin = End + 1;
		}
		return Items;
	};
	if (const std::wstring HoldKeys = CommandLine.GetValue(L"--hold-keys"); !HoldKeys.empty())
	{
		for (const std::string& Name : SplitList(HoldKeys))
		{
			EKey Key = EKey::None;
			if (InputNames::TryParseKey(Name, Key))
			{
				HeldKeys.push_back(Key);
			}
			else
			{
				E_LOG(LogCore, Warning, "--hold-keys: 알 수 없는 키 '{}'", Name);
			}
		}
		E_LOG(LogCore, Display, "--hold-keys: 키 {}개를 누른 상태로 실행", HeldKeys.size());
	}
	if (const std::wstring HoldPad = CommandLine.GetValue(L"--hold-gamepad"); !HoldPad.empty())
	{
		bHoldGamepad            = true;
		HeldGamepad.bConnected = true;
		for (const std::string& Item : SplitList(HoldPad))
		{
			const size_t         Equals = Item.find('=');
			const std::string    Name   = Item.substr(0, Equals);
			const float          Value  = Equals == std::string::npos ? 1.0f : std::strtof(Item.c_str() + Equals + 1, nullptr);
			EGamepadButton       Button;
			EGamepadAxis         Axis;
			if (InputNames::TryParseGamepadButton(Name, Button))
			{
				HeldGamepad.SetButton(Button, true);
			}
			else if (InputNames::TryParseGamepadAxis(Name, Axis) && Axis != EGamepadAxis::LeftStick && Axis != EGamepadAxis::RightStick)
			{
				float* const Targets[] = { nullptr, nullptr, &HeldGamepad.LeftX, &HeldGamepad.LeftY, &HeldGamepad.RightX, &HeldGamepad.RightY,
				                           &HeldGamepad.LeftTrigger, &HeldGamepad.RightTrigger };
				*Targets[static_cast<size_t>(Axis)] = std::clamp(Value, -1.0f, 1.0f);
			}
			else
			{
				E_LOG(LogCore, Warning, "--hold-gamepad: 알 수 없는 버튼/축 '{}'", Name);
			}
		}
		E_LOG(LogCore, Display, "--hold-gamepad: 가짜 게임패드 상태로 실행");
	}
	if (const std::wstring Delay = CommandLine.GetValue(L"--hold-keys-delay"); !Delay.empty())
	{
		HoldKeysDelay = std::max(0.0f, std::stof(Delay));
	}
	// 플레이어 입력 재지정 파일 (<Saved>/Config/InputBindings.json) — 자동 검증 실행은 읽지도 쓰지도 않는다
	FInputSettings& InputSettings = FProjectSettings::Get().Input;
	InputSettings.SetUserFileEnabled(ExitAfterFrames == 0 && !Desc.bHeadless);
	InputSettings.LoadUserBindings();
	if (CommandLine.HasFlag(L"--crash-test"))
	{
		CrashTestFrame = 30; // 패키지 크래시 덤프 검증: 30프레임(틱) 뒤 의도적 액세스 위반
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
		OnConfigureWindow(Desc.Window);
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
		UpdateHeldInputAndActions(Timer.GetDeltaSeconds());
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
			UpdateCrashTest();
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
		UpdateCrashTest();
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

void FApplication::UpdateHeldInputAndActions(float DeltaSeconds)
{
	if ((!HeldKeys.empty() || bHoldGamepad) && !bHoldKeysStarted && Timer.GetTotalSeconds() >= HoldKeysDelay)
	{
		bHoldKeysStarted = true;
		E_LOG(LogCore, Display, "--hold-keys: 키 누르기 시작");
	}
	for (const EKey Key : bHoldKeysStarted ? HeldKeys : std::vector<EKey>{}) // 자동 검증: 누르고 있는 키
	{
		FWindowEvent Event;
		Event.Type = EWindowEventType::KeyDown;
		Event.Key  = Key;
		Input.ProcessEvent(Event);
	}

	// 게임패드: 첫 번째 연결 패드 (창 포커스가 없으면 중립). 자동 검증 가짜 패드가 있으면 그것
	Gamepads.Poll(DeltaSeconds);
	FGamepadState Pad = Gamepads.GetPrimary();
	if (!bWindowFocused)
	{
		Pad = FGamepadState{ .bConnected = Pad.bConnected };
	}
	if (bHoldGamepad)
	{
		Pad = bHoldKeysStarted ? HeldGamepad : FGamepadState{ .bConnected = true };
	}
	Input.SetGamepadState(Pad);

	// 입력 액션 (유효 매핑 = 프로젝트 설정 "입력" + 플레이어 재지정)
	Input.UpdateActions(FProjectSettings::Get().Input.GetEffectiveMapping(), DeltaSeconds);
}

void FApplication::UpdateCrashTest()
{
	if (CrashTestFrame != 0 && FrameIndex == CrashTestFrame)
	{
		E_LOG(LogCore, Warning, "--crash-test: 의도적으로 크래시를 일으킵니다");
		volatile int* Null = nullptr;
		*Null = 1;
	}
}

void FApplication::HandleWindowEvent(const FWindowEvent& Event)
{
	Input.ProcessEvent(Event);
	if (Event.Type == EWindowEventType::Focus)
	{
		bWindowFocused = Event.bFocused;
	}

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

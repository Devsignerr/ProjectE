#pragma once

#include "Core/CoreTypes.h"
#include "Core/Input.h"
#include "Core/Timer.h"
#include "Core/Window.h"

#include <filesystem>

struct FApplicationDesc
{
	FWindowDesc Window;
};

// 애플리케이션 기본 클래스: 창/입력/타이머를 소유하고 메인 루프를 돈다.
// 파생 클래스는 On* 훅을 재정의한다. 자동 검증 명령줄 인자(--log/--exit-after/--screenshot)를 공통 처리한다.
class FApplication
{
public:
	explicit FApplication(const FApplicationDesc& InDesc);
	virtual ~FApplication() = default;

	FApplication(const FApplication&)            = delete;
	FApplication& operator=(const FApplication&) = delete;

	// 초기화 → 메인 루프 → 종료. 프로세스 종료 코드를 반환.
	int Run();

	void RequestExit() { bExitRequested = true; }

	FWindow&      GetWindow() { return Window; }
	const FInput& GetInput() const { return Input; }
	const FTimer& GetTimer() const { return Timer; }

protected:
	virtual bool OnInit() { return true; }
	virtual void OnUpdate(float /*DeltaSeconds*/) {}
	virtual void OnRender() {}
	virtual void OnResize(uint32 /*Width*/, uint32 /*Height*/) {}
	virtual void OnShutdown() {}

	// 자동 검증: --screenshot 요청 시 이번 프레임 렌더 직전에 호출된다. 파생 클래스는 RHI에 전달한다.
	virtual void OnScreenshotRequested(const std::filesystem::path& /*Path*/) {}

	// 자동 검증 실행(--exit-after/--screenshot) 중인지. 오디오 음소거 등에 사용
	bool IsAutomationRun() const { return ExitAfterFrames > 0; }

	// 현재까지 렌더한 프레임 수
	uint64 GetFrameIndex() const { return FrameIndex; }

private:
	void HandleWindowEvent(const FWindowEvent& Event);

	FApplicationDesc Desc;
	FWindow          Window;
	FInput           Input;
	FTimer           Timer;
	bool             bExitRequested = false;

	// 자동 검증 인자 (명령줄):
	//   --log <경로>          모든 로그를 파일에도 기록
	//   --exit-after <N>      N 프레임 렌더 후 종료 (--screenshot만 주면 기본 90)
	//   --screenshot <경로>   마지막 프레임을 PNG로 저장
	uint64                ExitAfterFrames = 0;
	std::filesystem::path ScreenshotPath;
	uint64                FrameIndex = 0;
};

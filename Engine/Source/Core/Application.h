#pragma once

#include "Core/CoreTypes.h"
#include "Core/Input.h"
#include "Core/Timer.h"
#include "Core/Window.h"

#include <atomic>
#include <filesystem>

struct FApplicationDesc
{
	FWindowDesc Window;

	// 창/렌더 없이 틱만 도는 실행 (전용 서버). OnRender/OnResize는 불리지 않고 --exit-after는 틱 수로 센다.
	// 콘솔 Ctrl+C / 창 닫기는 RequestExit로 처리한다
	bool  bHeadless           = false;
	float HeadlessTickSeconds = 1.0f / 60.0f; // 한 틱 목표 간격 (남는 시간은 대기)
};

// 애플리케이션 기본 클래스: 창/입력/타이머를 소유하고 메인 루프를 돈다.
// 파생 클래스는 On* 훅을 재정의한다. 자동 검증 명령줄 인자(--log/--exit-after/--screenshot)를 공통 처리한다.
// FApplicationDesc::bHeadless면 창을 만들지 않고 고정 간격 틱 루프를 돈다 (전용 서버).
class FApplication
{
public:
	explicit FApplication(const FApplicationDesc& InDesc);
	virtual ~FApplication() = default;

	FApplication(const FApplication&)            = delete;
	FApplication& operator=(const FApplication&) = delete;

	// 초기화 → 메인 루프 → 종료. 프로세스 종료 코드를 반환.
	int Run();

	void RequestExit() { bExitRequested = true; } // 다른 스레드(콘솔 Ctrl+C 처리기)에서 불려도 된다
	bool IsHeadless() const { return Desc.bHeadless; }

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

	// 현재까지 렌더한 프레임 수 (헤드리스는 틱 수)
	uint64 GetFrameIndex() const { return FrameIndex; }

private:
	void HandleWindowEvent(const FWindowEvent& Event);
	void RunWindowedLoop();
	void RunHeadlessLoop();
	void UpdateCrashTest();

	FApplicationDesc Desc;
	FWindow          Window;
	FInput           Input;
	FTimer           Timer;
	std::atomic<bool> bExitRequested = false;

	// 자동 검증 인자 (명령줄):
	//   --log <경로>          모든 로그를 파일에도 기록 (없으면 <Saved>/Logs/<실행 파일 이름>.log)
	//   --exit-after <N>      N 프레임 렌더 후 종료 (--screenshot만 주면 기본 90, 헤드리스는 N 틱)
	//   --screenshot <경로>   마지막 프레임을 PNG로 저장
	//   --crash-test          30프레임 뒤 액세스 위반을 일으켜 크래시 덤프/대화 상자를 검증
	uint64                ExitAfterFrames = 0;
	std::filesystem::path ScreenshotPath;
	uint64                FrameIndex = 0;
	uint64                CrashTestFrame = 0; // --crash-test: 이 프레임(틱)에서 의도적 크래시 (덤프 검증)
};

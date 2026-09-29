#pragma once

#include "Core/CoreTypes.h"
#include "Core/Input.h"
#include "Core/Timer.h"
#include "Core/Window.h"

struct FApplicationDesc
{
	FWindowDesc Window;
};

// 애플리케이션 기본 클래스: 창/입력/타이머를 소유하고 메인 루프를 돈다.
// 파생 클래스는 On* 훅을 재정의한다.
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

private:
	void HandleWindowEvent(const FWindowEvent& Event);

	FApplicationDesc Desc;
	FWindow          Window;
	FInput           Input;
	FTimer           Timer;
	bool             bExitRequested = false;
};

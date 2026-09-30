#pragma once

#include "Core/CoreTypes.h"
#include "Core/WindowEvent.h"

#include <functional>
#include <string>

// Windows.h 없이 창 핸들 전방 선언
struct HWND__;
using HWND = HWND__*;

struct FWindowDesc
{
	std::wstring Title      = L"ProjectE";
	uint32       Width      = 1280;
	uint32       Height     = 720;
	bool         bResizable = true;
};

// Win32 최상위 창. 메시지를 FWindowEvent로 변환해 핸들러에 전달한다.
class FWindow
{
public:
	using FEventHandler = std::function<void(const FWindowEvent&)>;
	// 원시 Win32 메시지를 먼저 받는 훅 (UI 라이브러리 등). true를 반환하면 메시지를 소비한다.
	using FMessageHook = std::function<bool(HWND Hwnd, uint32 Message, uint64 WParam, int64 LParam)>;

	FWindow() = default;
	~FWindow();

	FWindow(const FWindow&)            = delete;
	FWindow& operator=(const FWindow&) = delete;

	bool Create(const FWindowDesc& Desc);
	void Destroy();

	// 대기 중인 모든 메시지를 처리한다. 매 프레임 호출.
	void PumpMessages();

	void SetEventHandler(FEventHandler Handler) { EventHandler = std::move(Handler); }
	void SetMessageHook(FMessageHook Hook) { MessageHook = std::move(Hook); }
	void SetTitle(const std::wstring& Title);

	// 테두리 없는 전체 화면 (창이 있는 모니터 전체를 덮는 팝업 창). 끄면 이전 창 위치/크기/최대화 상태로 돌아간다.
	// 크기가 바뀌므로 Resize 이벤트가 온다
	void SetBorderlessFullscreen(bool bEnable);
	bool IsBorderlessFullscreen() const { return bBorderlessFullscreen; }

	HWND   GetHandle() const { return Hwnd; }
	uint32 GetWidth() const { return Width; }
	uint32 GetHeight() const { return Height; }
	bool   IsMinimized() const { return bMinimized; }

private:
	// Win32 WNDPROC 시그니처와 동일한 타입 (LRESULT/UINT/WPARAM/LPARAM)
	static int64 __stdcall WndProc(HWND InHwnd, uint32 Message, uint64 WParam, int64 LParam);
	int64 HandleMessage(uint32 Message, uint64 WParam, int64 LParam);

	void Dispatch(const FWindowEvent& Event);

	HWND          Hwnd        = nullptr;
	uint32        Width       = 0;
	uint32        Height      = 0;
	bool          bMinimized  = false;
	bool          bInSizeMove = false; // 드래그 리사이즈 중에는 Resize 이벤트를 보류
	uint32        PendingHighSurrogate = 0; // WM_CHAR UTF-16 서로게이트 앞쪽
	bool          bBorderlessFullscreen = false;
	// 전체 화면 전 창 상태 (WINDOWPLACEMENT 일부 — 헤더에 Windows.h를 넣지 않으려고 값으로 보관)
	uint32        SavedStyle      = 0;
	int32         SavedNormalRect[4] = {}; // left, top, right, bottom
	bool          bSavedMaximized = false;
	FEventHandler EventHandler;
	FMessageHook  MessageHook;
};

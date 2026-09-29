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
	FEventHandler EventHandler;
	FMessageHook  MessageHook;
};

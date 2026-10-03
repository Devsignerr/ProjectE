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
	bool         bHidden    = false; // 화면에 띄우지 않고 그리기만 (자동 검증 — 작업표시줄·포커스를 빼앗지 않는다)
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

	// 커서 잠금 (FPS 시점): 커서를 숨기고 창 안에 가둔다. 포커스를 잃으면 자동으로 풀린다. 시점 회전은 원시 입력(RawMouseMove)으로 받는다
	void SetCursorLocked(bool bLock);
	bool IsCursorLocked() const { return bCursorLocked; }
	// 잠금 중 커서를 둘 점 (클라이언트 픽셀 — 에디터는 뷰포트 가운데). 음수 = 클라이언트 영역 가운데 (기본). 매 프레임 불러도 된다
	void SetCursorLockPoint(int32 X, int32 Y);

	// 게임 UI 텍스트 입력 (매 프레임 불러도 된다). 켜져 있으면 IME 조합을 창이 직접 받아 ImeComposition/Char 이벤트로 보내고
	// (시스템 조합 창 대신 UI가 그린다), 조합/후보 창을 캐럿(클라이언트 픽셀: 위쪽 X/Y, 높이) 아래에 둔다. 끄면 조합 중인 글자는 취소된다
	void SetTextInput(bool bActive, int32 CaretX, int32 CaretY, int32 CaretHeight);
	bool IsTextInputActive() const { return bTextInput; }

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
	bool          bHidden               = false; // FWindowDesc::bHidden — 창 모드 전환도 보이게 하지 않는다
	bool          bCursorLocked         = false;
	int32         CursorLockPoint[2]    = { -1, -1 }; // SetCursorLockPoint
	void          ApplyCursorClip() const; // 잠금 중이면 클라이언트 영역에 가둔다 (크기/위치가 바뀔 때마다)
	// 전체 화면 전 창 상태 (WINDOWPLACEMENT 일부 — 헤더에 Windows.h를 넣지 않으려고 값으로 보관)
	uint32        SavedStyle      = 0;
	int32         SavedNormalRect[4] = {}; // left, top, right, bottom
	bool          bSavedMaximized = false;
	FEventHandler EventHandler;
	FMessageHook  MessageHook;

	// IME (SetTextInput)
	bool           bTextInput      = false;
	int32          TextCaret[3]    = { -1, -1, -1 }; // 마지막으로 IME에 알린 캐럿 (X, Y, 높이)
	std::u32string ImeComposition;                   // ImeComposition 이벤트가 가리키는 버퍼
	void           HandleImeComposition(int64 LParam);
	void           DispatchComposition(int32 Cursor);
};

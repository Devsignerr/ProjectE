#include "Core/Window.h"

#include "Core/Assert.h"
#include "Core/Platform/WindowsHeaders.h"

#include <imm.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "imm32") // IME 조합 (게임 UI 텍스트 입력)

namespace
{
	constexpr const wchar_t* GWindowClassName = L"ProjectEWindowClass";

	// Win32 가상 키 코드를 EKey로 변환
	EKey TranslateKey(uint64 VirtualKey, int64 LParam)
	{
		const bool bExtended = (LParam & (1LL << 24)) != 0;

		if (VirtualKey >= 'A' && VirtualKey <= 'Z')
		{
			return static_cast<EKey>(static_cast<uint16>(EKey::A) + (VirtualKey - 'A'));
		}
		if (VirtualKey >= '0' && VirtualKey <= '9')
		{
			return static_cast<EKey>(static_cast<uint16>(EKey::Zero) + (VirtualKey - '0'));
		}
		if (VirtualKey >= VK_F1 && VirtualKey <= VK_F12)
		{
			return static_cast<EKey>(static_cast<uint16>(EKey::F1) + (VirtualKey - VK_F1));
		}
		if (VirtualKey >= VK_NUMPAD0 && VirtualKey <= VK_NUMPAD9)
		{
			return static_cast<EKey>(static_cast<uint16>(EKey::Numpad0) + (VirtualKey - VK_NUMPAD0));
		}

		switch (VirtualKey)
		{
		case VK_SHIFT:
		{
			// 좌/우 Shift는 스캔 코드로 구분
			const UINT ScanCode = static_cast<UINT>((LParam >> 16) & 0xFF);
			return MapVirtualKeyW(ScanCode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? EKey::RightShift : EKey::LeftShift;
		}
		case VK_CONTROL:    return bExtended ? EKey::RightControl : EKey::LeftControl;
		case VK_MENU:       return bExtended ? EKey::RightAlt : EKey::LeftAlt;
		case VK_RETURN:     return bExtended ? EKey::NumpadEnter : EKey::Enter;

		case VK_ESCAPE:     return EKey::Escape;
		case VK_TAB:        return EKey::Tab;
		case VK_CAPITAL:    return EKey::CapsLock;
		case VK_SPACE:      return EKey::Space;
		case VK_BACK:       return EKey::Backspace;

		case VK_INSERT:     return EKey::Insert;
		case VK_DELETE:     return EKey::Delete;
		case VK_HOME:       return EKey::Home;
		case VK_END:        return EKey::End;
		case VK_PRIOR:      return EKey::PageUp;
		case VK_NEXT:       return EKey::PageDown;
		case VK_LEFT:       return EKey::Left;
		case VK_RIGHT:      return EKey::Right;
		case VK_UP:         return EKey::Up;
		case VK_DOWN:       return EKey::Down;

		case VK_ADD:        return EKey::NumpadAdd;
		case VK_SUBTRACT:   return EKey::NumpadSubtract;
		case VK_MULTIPLY:   return EKey::NumpadMultiply;
		case VK_DIVIDE:     return EKey::NumpadDivide;
		case VK_DECIMAL:    return EKey::NumpadDecimal;

		case VK_OEM_MINUS:  return EKey::Minus;
		case VK_OEM_PLUS:   return EKey::Equals;
		case VK_OEM_4:      return EKey::LeftBracket;
		case VK_OEM_6:      return EKey::RightBracket;
		case VK_OEM_5:      return EKey::Backslash;
		case VK_OEM_1:      return EKey::Semicolon;
		case VK_OEM_7:      return EKey::Apostrophe;
		case VK_OEM_COMMA:  return EKey::Comma;
		case VK_OEM_PERIOD: return EKey::Period;
		case VK_OEM_2:      return EKey::Slash;
		case VK_OEM_3:      return EKey::Grave;

		default:            return EKey::None;
		}
	}

	int32 GetMouseX(int64 LParam) { return static_cast<int16>(LOWORD(LParam)); }
	int32 GetMouseY(int64 LParam) { return static_cast<int16>(HIWORD(LParam)); }
} // namespace

FWindow::~FWindow()
{
	Destroy();
}

bool FWindow::Create(const FWindowDesc& Desc)
{
	E_CHECKF(Hwnd == nullptr, "창이 이미 생성되어 있습니다");

	const HINSTANCE Instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW WindowClass{};
	WindowClass.cbSize        = sizeof(WNDCLASSEXW);
	WindowClass.style         = CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc   = &FWindow::WndProc;
	WindowClass.hInstance     = Instance;
	WindowClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
	// 패키지 exe에 써 넣은 아이콘 그룹 1 (FExecutableResources::IconGroupId). 없으면 기본 아이콘
	WindowClass.hIcon   = static_cast<HICON>(LoadImageW(Instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
	WindowClass.hIconSm = static_cast<HICON>(LoadImageW(Instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
	WindowClass.hbrBackground = nullptr; // 배경은 렌더러가 그린다 (깜빡임 방지)
	WindowClass.lpszClassName = GWindowClassName;

	// 여러 창을 만들 수 있으므로 이미 등록된 클래스는 허용
	if (!RegisterClassExW(&WindowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		E_LOG(LogCore, Error, "창 클래스 등록 실패 (오류 코드 {})", GetLastError());
		return false;
	}

	DWORD Style = WS_OVERLAPPEDWINDOW;
	if (!Desc.bResizable)
	{
		Style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
	}

	// 클라이언트 영역이 요청 크기가 되도록 창 크기 보정
	RECT Rect{ 0, 0, static_cast<LONG>(Desc.Width), static_cast<LONG>(Desc.Height) };
	AdjustWindowRectEx(&Rect, Style, FALSE, 0);

	Width  = Desc.Width;
	Height = Desc.Height;

	// WndProc은 WM_NCCREATE에서 lpCreateParams(this)를 통해 Hwnd를 설정한다
	const HWND Created = CreateWindowExW(0, GWindowClassName, Desc.Title.c_str(), Style,
	                                     CW_USEDEFAULT, CW_USEDEFAULT,
	                                     Rect.right - Rect.left, Rect.bottom - Rect.top,
	                                     nullptr, nullptr, Instance, this);
	if (Created == nullptr)
	{
		E_LOG(LogCore, Error, "창 생성 실패 (오류 코드 {})", GetLastError());
		Hwnd = nullptr;
		return false;
	}

	bHidden = Desc.bHidden;
	if (!bHidden)
	{
		ShowWindow(Hwnd, SW_SHOW);
	}

	// 원시 마우스 입력 (시점 회전용 이동량 — 포그라운드일 때만 받는다)
	RAWINPUTDEVICE Mouse{};
	Mouse.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
	Mouse.usUsage     = 0x02; // HID_USAGE_GENERIC_MOUSE
	Mouse.hwndTarget  = Hwnd;
	if (!RegisterRawInputDevices(&Mouse, 1, sizeof(Mouse)))
	{
		E_LOG(LogCore, Warning, "원시 마우스 입력 등록 실패 (오류 코드 {}) — 시점 회전이 동작하지 않습니다", GetLastError());
	}

	// 실제 클라이언트 크기로 갱신 (DPI 등으로 요청과 다를 수 있음)
	RECT ClientRect{};
	if (GetClientRect(Hwnd, &ClientRect))
	{
		Width  = static_cast<uint32>(ClientRect.right - ClientRect.left);
		Height = static_cast<uint32>(ClientRect.bottom - ClientRect.top);
	}

	E_LOG(LogCore, Display, "창 생성 완료: {}x{}", Width, Height);
	return true;
}

void FWindow::Destroy()
{
	if (Hwnd != nullptr)
	{
		DestroyWindow(Hwnd);
		Hwnd = nullptr;
	}
}

void FWindow::PumpMessages()
{
	MSG Message{};
	while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
}

void FWindow::SetTitle(const std::wstring& Title)
{
	if (Hwnd != nullptr)
	{
		SetWindowTextW(Hwnd, Title.c_str());
	}
}

void FWindow::SetBorderlessFullscreen(bool bEnable)
{
	if (Hwnd == nullptr || bEnable == bBorderlessFullscreen)
	{
		return;
	}
	bBorderlessFullscreen = bEnable;

	if (bEnable)
	{
		WINDOWPLACEMENT Placement{};
		Placement.length = sizeof(Placement);
		GetWindowPlacement(Hwnd, &Placement);
		SavedStyle         = static_cast<uint32>(GetWindowLongPtrW(Hwnd, GWL_STYLE));
		SavedNormalRect[0] = Placement.rcNormalPosition.left;
		SavedNormalRect[1] = Placement.rcNormalPosition.top;
		SavedNormalRect[2] = Placement.rcNormalPosition.right;
		SavedNormalRect[3] = Placement.rcNormalPosition.bottom;
		bSavedMaximized    = Placement.showCmd == SW_SHOWMAXIMIZED;

		MONITORINFO Monitor{};
		Monitor.cbSize = sizeof(Monitor);
		GetMonitorInfoW(MonitorFromWindow(Hwnd, MONITOR_DEFAULTTONEAREST), &Monitor);
		const LONG_PTR Visible = bHidden ? 0 : WS_VISIBLE;
		SetWindowLongPtrW(Hwnd, GWL_STYLE, static_cast<LONG_PTR>((SavedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP) | Visible);
		SetWindowPos(Hwnd, HWND_TOP, Monitor.rcMonitor.left, Monitor.rcMonitor.top,
		             Monitor.rcMonitor.right - Monitor.rcMonitor.left, Monitor.rcMonitor.bottom - Monitor.rcMonitor.top,
		             SWP_FRAMECHANGED | SWP_NOOWNERZORDER | (bHidden ? SWP_NOACTIVATE : SWP_SHOWWINDOW));
		E_LOG(LogCore, Display, "테두리 없는 전체 화면: {}x{}", Monitor.rcMonitor.right - Monitor.rcMonitor.left, Monitor.rcMonitor.bottom - Monitor.rcMonitor.top);
	}
	else
	{
		SetWindowLongPtrW(Hwnd, GWL_STYLE, static_cast<LONG_PTR>(SavedStyle));
		WINDOWPLACEMENT Placement{};
		Placement.length                  = sizeof(Placement);
		Placement.showCmd                 = bHidden ? SW_HIDE : (bSavedMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
		Placement.rcNormalPosition.left   = SavedNormalRect[0];
		Placement.rcNormalPosition.top    = SavedNormalRect[1];
		Placement.rcNormalPosition.right  = SavedNormalRect[2];
		Placement.rcNormalPosition.bottom = SavedNormalRect[3];
		SetWindowPlacement(Hwnd, &Placement);
		SetWindowPos(Hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
		E_LOG(LogCore, Display, "창 모드로 전환");
	}
}

void FWindow::SetCursorLocked(bool bLock)
{
	if (Hwnd == nullptr || bLock == bCursorLocked)
	{
		return;
	}
	if (bLock && GetForegroundWindow() != Hwnd)
	{
		return; // 다른 창이 앞에 있으면 잠그지 않는다
	}
	bCursorLocked = bLock;
	ShowCursor(bLock ? FALSE : TRUE);
	if (bLock)
	{
		ApplyCursorClip();
	}
	else
	{
		ClipCursor(nullptr);
	}
}

void FWindow::SetCursorLockPoint(int32 X, int32 Y)
{
	if (CursorLockPoint[0] == X && CursorLockPoint[1] == Y)
	{
		return;
	}
	CursorLockPoint[0] = X;
	CursorLockPoint[1] = Y;
	ApplyCursorClip();
}

void FWindow::ApplyCursorClip() const
{
	if (!bCursorLocked)
	{
		return;
	}
	RECT Client{};
	GetClientRect(Hwnd, &Client);
	POINT TopLeft{ Client.left, Client.top };
	POINT BottomRight{ Client.right, Client.bottom };
	ClientToScreen(Hwnd, &TopLeft);
	ClientToScreen(Hwnd, &BottomRight);
	// 가운데 한 점에 가둔다 (커서가 창 가장자리 UI에 걸리지 않게). 지정한 점이 있으면 그 점 (클라이언트 영역 안으로 자름)
	const bool bCustomPoint = CursorLockPoint[0] >= 0 && CursorLockPoint[1] >= 0;
	const LONG CenterX      = bCustomPoint ? std::min<LONG>(TopLeft.x + CursorLockPoint[0], BottomRight.x - 1) : (TopLeft.x + BottomRight.x) / 2;
	const LONG CenterY      = bCustomPoint ? std::min<LONG>(TopLeft.y + CursorLockPoint[1], BottomRight.y - 1) : (TopLeft.y + BottomRight.y) / 2;
	const RECT Clip{ CenterX, CenterY, CenterX + 1, CenterY + 1 };
	ClipCursor(&Clip);
}

void FWindow::Dispatch(const FWindowEvent& Event)
{
	if (EventHandler)
	{
		EventHandler(Event);
	}
}

int64 __stdcall FWindow::WndProc(HWND InHwnd, uint32 Message, uint64 WParam, int64 LParam)
{
	FWindow* Self = nullptr;

	if (Message == WM_NCCREATE)
	{
		const auto* CreateInfo = reinterpret_cast<const CREATESTRUCTW*>(LParam);
		Self = static_cast<FWindow*>(CreateInfo->lpCreateParams);
		Self->Hwnd = InHwnd;
		SetWindowLongPtrW(InHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
	}
	else
	{
		Self = reinterpret_cast<FWindow*>(GetWindowLongPtrW(InHwnd, GWLP_USERDATA));
	}

	if (Self != nullptr)
	{
		return Self->HandleMessage(Message, WParam, LParam);
	}
	return DefWindowProcW(InHwnd, Message, WParam, LParam);
}

void FWindow::SetTextInput(bool bActive, int32 CaretX, int32 CaretY, int32 CaretHeight)
{
	if (Hwnd == nullptr)
	{
		return;
	}
	if (!bActive)
	{
		if (bTextInput)
		{
			bTextInput = false;
			TextCaret[0] = TextCaret[1] = TextCaret[2] = -1;
			if (HIMC Himc = ImmGetContext(Hwnd))
			{
				ImmNotifyIME(Himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0); // 포커스를 잃은 입력칸의 조합 중 글자는 버린다
				ImmReleaseContext(Hwnd, Himc);
			}
			if (!ImeComposition.empty())
			{
				ImeComposition.clear();
				DispatchComposition(0);
			}
		}
		return;
	}
	bTextInput = true;
	if (TextCaret[0] == CaretX && TextCaret[1] == CaretY && TextCaret[2] == CaretHeight)
	{
		return;
	}
	TextCaret[0] = CaretX;
	TextCaret[1] = CaretY;
	TextCaret[2] = CaretHeight;
	if (HIMC Himc = ImmGetContext(Hwnd))
	{
		COMPOSITIONFORM Composition = {};
		Composition.dwStyle         = CFS_FORCE_POSITION;
		Composition.ptCurrentPos    = { CaretX, CaretY + CaretHeight };
		ImmSetCompositionWindow(Himc, &Composition);
		CANDIDATEFORM Candidate = {};
		Candidate.dwIndex       = 0;
		Candidate.dwStyle       = CFS_EXCLUDE; // 캐럿 줄을 가리지 않게 아래에
		Candidate.ptCurrentPos  = { CaretX, CaretY + CaretHeight };
		Candidate.rcArea        = { CaretX, CaretY, CaretX + 1, CaretY + CaretHeight };
		ImmSetCandidateWindow(Himc, &Candidate);
		ImmReleaseContext(Hwnd, Himc);
	}
}

void FWindow::DispatchComposition(int32 Cursor)
{
	FWindowEvent Event{};
	Event.Type              = EWindowEventType::ImeComposition;
	Event.Composition       = ImeComposition.data();
	Event.CompositionLength = static_cast<uint32>(ImeComposition.size());
	Event.CompositionCursor = Cursor;
	Dispatch(Event);
}

void FWindow::HandleImeComposition(int64 LParam)
{
	HIMC Himc = ImmGetContext(Hwnd);
	if (Himc == nullptr)
	{
		return;
	}
	// UTF-16 → 코드 포인트. OutUnitsToPoints[i] = i번째 UTF-16 단위 앞까지의 코드 포인트 수
	const auto ReadString = [Himc](DWORD Index, std::u32string& Out, std::vector<int32>* OutUnitsToPoints) {
		Out.clear();
		const LONG Bytes = ImmGetCompositionStringW(Himc, Index, nullptr, 0);
		if (Bytes <= 0)
		{
			if (OutUnitsToPoints != nullptr)
			{
				OutUnitsToPoints->assign(1, 0);
			}
			return;
		}
		std::wstring Wide(static_cast<size_t>(Bytes) / sizeof(wchar_t), L'\0');
		ImmGetCompositionStringW(Himc, Index, Wide.data(), static_cast<DWORD>(Bytes));
		if (OutUnitsToPoints != nullptr)
		{
			OutUnitsToPoints->clear();
		}
		for (size_t Unit = 0; Unit < Wide.size(); ++Unit)
		{
			if (OutUnitsToPoints != nullptr)
			{
				OutUnitsToPoints->push_back(static_cast<int32>(Out.size()));
			}
			const uint32 Code = static_cast<uint32>(Wide[Unit]);
			if (Code >= 0xD800 && Code <= 0xDBFF && Unit + 1 < Wide.size())
			{
				const uint32 Low = static_cast<uint32>(Wide[Unit + 1]);
				Out.push_back(static_cast<char32_t>(0x10000 + ((Code - 0xD800) << 10) + (Low - 0xDC00)));
				++Unit;
				if (OutUnitsToPoints != nullptr)
				{
					OutUnitsToPoints->push_back(static_cast<int32>(Out.size()));
				}
				continue;
			}
			Out.push_back(static_cast<char32_t>(Code));
		}
		if (OutUnitsToPoints != nullptr)
		{
			OutUnitsToPoints->push_back(static_cast<int32>(Out.size()));
		}
	};

	// 확정 글자 먼저 (WM_CHAR와 같은 Char 이벤트 — DefWindowProc에 넘기지 않으므로 WM_CHAR는 오지 않는다)
	if ((LParam & GCS_RESULTSTR) != 0)
	{
		std::u32string Result;
		ReadString(GCS_RESULTSTR, Result, nullptr);
		for (const char32_t Char : Result)
		{
			FWindowEvent Event{};
			Event.Type      = EWindowEventType::Char;
			Event.Character = static_cast<uint32>(Char);
			Dispatch(Event);
		}
	}
	if ((LParam & GCS_COMPSTR) != 0)
	{
		std::vector<int32> UnitsToPoints;
		ReadString(GCS_COMPSTR, ImeComposition, &UnitsToPoints);
		int32 Cursor = static_cast<int32>(ImeComposition.size());
		if ((LParam & GCS_CURSORPOS) != 0)
		{
			const LONG Units = ImmGetCompositionStringW(Himc, GCS_CURSORPOS, nullptr, 0);
			if (Units >= 0 && static_cast<size_t>(Units) < UnitsToPoints.size())
			{
				Cursor = UnitsToPoints[static_cast<size_t>(Units)];
			}
		}
		DispatchComposition(Cursor);
	}
	else if ((LParam & GCS_RESULTSTR) != 0 && !ImeComposition.empty())
	{
		ImeComposition.clear();
		DispatchComposition(0);
	}
	ImmReleaseContext(Hwnd, Himc);
}

int64 FWindow::HandleMessage(uint32 Message, uint64 WParam, int64 LParam)
{
	// 게임 UI 텍스트 입력 중: IME 조합을 직접 받는다 (UI 훅(ImGui)보다 먼저 — 에디터 텍스트 필드는 이때 입력을 받지 않는다)
	if (bTextInput)
	{
		switch (Message)
		{
		case WM_IME_STARTCOMPOSITION:
			return 0; // DefWindowProc에 넘기지 않으면 시스템 조합 창이 뜨지 않는다
		case WM_IME_COMPOSITION:
			HandleImeComposition(LParam);
			return 0;
		case WM_IME_ENDCOMPOSITION:
			if (!ImeComposition.empty())
			{
				ImeComposition.clear();
				DispatchComposition(0);
			}
			return 0;
		default:
			break;
		}
	}

	if (MessageHook && MessageHook(Hwnd, Message, WParam, LParam))
	{
		return 1;
	}

	FWindowEvent Event{};

	switch (Message)
	{
	case WM_CLOSE:
		// 창 파괴는 애플리케이션이 결정한다
		Event.Type = EWindowEventType::Close;
		Dispatch(Event);
		return 0;

	case WM_SIZE:
		ApplyCursorClip();
		Width      = LOWORD(LParam);
		Height     = HIWORD(LParam);
		bMinimized = (WParam == SIZE_MINIMIZED);
		if (!bInSizeMove)
		{
			Event.Type       = EWindowEventType::Resize;
			Event.Width      = Width;
			Event.Height     = Height;
			Event.bMinimized = bMinimized;
			Dispatch(Event);
		}
		return 0;

	case WM_ENTERSIZEMOVE:
		bInSizeMove = true;
		return 0;

	case WM_EXITSIZEMOVE:
		bInSizeMove      = false;
		Event.Type       = EWindowEventType::Resize;
		Event.Width      = Width;
		Event.Height     = Height;
		Event.bMinimized = bMinimized;
		Dispatch(Event);
		return 0;

	case WM_GETMINMAXINFO:
	{
		// 클라이언트 영역이 0이 되지 않도록 최소 크기 제한
		auto* MinMax = reinterpret_cast<MINMAXINFO*>(LParam);
		MinMax->ptMinTrackSize.x = 320;
		MinMax->ptMinTrackSize.y = 240;
		return 0;
	}

	case WM_INPUT:
	{
		RAWINPUT Raw{};
		UINT     Size = sizeof(Raw);
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(LParam), RID_INPUT, &Raw, &Size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
		    Raw.header.dwType == RIM_TYPEMOUSE && (Raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
		    (Raw.data.mouse.lLastX != 0 || Raw.data.mouse.lLastY != 0))
		{
			Event.Type   = EWindowEventType::RawMouseMove;
			Event.MouseX = Raw.data.mouse.lLastX;
			Event.MouseY = Raw.data.mouse.lLastY;
			Dispatch(Event);
		}
		break; // DefWindowProc가 정리한다
	}

	case WM_MOVE:
		ApplyCursorClip();
		break;

	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		if (Message == WM_KILLFOCUS)
		{
			SetCursorLocked(false); // 다른 창으로 가면 커서를 돌려준다
		}
		Event.Type     = EWindowEventType::Focus;
		Event.bFocused = (Message == WM_SETFOCUS);
		Dispatch(Event);
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		Event.Type    = EWindowEventType::KeyDown;
		Event.Key     = TranslateKey(WParam, LParam);
		Event.bRepeat = (LParam & (1LL << 30)) != 0;
		Dispatch(Event);
		// Alt+F4 등 시스템 키 기본 동작은 유지. F10 단독은 창 메뉴 모드로 들어가 다음 키를 삼키므로 넘기지 않는다 (디버거 단계 실행 키)
		return Message == WM_SYSKEYDOWN && WParam != VK_F10 ? DefWindowProcW(Hwnd, Message, WParam, LParam) : 0;

	case WM_CHAR:
	{
		// UTF-16: 서로게이트 쌍은 두 메시지로 온다
		const uint32 Unit = static_cast<uint32>(WParam);
		if (Unit >= 0xD800 && Unit <= 0xDBFF)
		{
			PendingHighSurrogate = Unit;
			return 0;
		}
		Event.Type      = EWindowEventType::Char;
		Event.Character = Unit;
		if (Unit >= 0xDC00 && Unit <= 0xDFFF && PendingHighSurrogate != 0)
		{
			Event.Character = 0x10000 + ((PendingHighSurrogate - 0xD800) << 10) + (Unit - 0xDC00);
		}
		PendingHighSurrogate = 0;
		Dispatch(Event);
		return 0;
	}

	case WM_SYSCHAR:
		// Alt+Enter는 앱이 전체 화면 전환으로 쓴다 (기본 처리는 경고음)
		if (WParam == VK_RETURN)
		{
			return 0;
		}
		break;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		Event.Type = EWindowEventType::KeyUp;
		Event.Key  = TranslateKey(WParam, LParam);
		Dispatch(Event);
		return Message == WM_SYSKEYUP && WParam != VK_F10 ? DefWindowProcW(Hwnd, Message, WParam, LParam) : 0;

	case WM_MOUSEMOVE:
		Event.Type   = EWindowEventType::MouseMove;
		Event.MouseX = GetMouseX(LParam);
		Event.MouseY = GetMouseY(LParam);
		Dispatch(Event);
		return 0;

	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP:
	{
		const bool bDown = (Message == WM_LBUTTONDOWN || Message == WM_RBUTTONDOWN ||
		                    Message == WM_MBUTTONDOWN || Message == WM_XBUTTONDOWN);

		switch (Message)
		{
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: Event.Button = EMouseButton::Left; break;
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: Event.Button = EMouseButton::Right; break;
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: Event.Button = EMouseButton::Middle; break;
		default:
			Event.Button = (HIWORD(WParam) == XBUTTON1) ? EMouseButton::Thumb1 : EMouseButton::Thumb2;
			break;
		}

		// 버튼을 누른 채 창 밖으로 나가도 이벤트를 받도록 캡처
		if (bDown)
		{
			SetCapture(Hwnd);
		}
		else
		{
			ReleaseCapture();
		}

		Event.Type   = bDown ? EWindowEventType::MouseButtonDown : EWindowEventType::MouseButtonUp;
		Event.MouseX = GetMouseX(LParam);
		Event.MouseY = GetMouseY(LParam);
		Dispatch(Event);
		return 0;
	}

	case WM_MOUSEWHEEL:
		Event.Type       = EWindowEventType::MouseWheel;
		Event.WheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(WParam)) / static_cast<float>(WHEEL_DELTA);
		Event.MouseX     = GetMouseX(LParam);
		Event.MouseY     = GetMouseY(LParam);
		Dispatch(Event);
		return 0;

	case WM_MENUCHAR:
		// Alt+Enter 등에서 경고음 방지
		return MAKELRESULT(0, MNC_CLOSE);

	default:
		break;
	}

	return DefWindowProcW(Hwnd, Message, WParam, LParam);
}

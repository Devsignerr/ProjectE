#include "Core/Window.h"

#include "Core/Assert.h"
#include "Core/Platform/WindowsHeaders.h"

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

	ShowWindow(Hwnd, SW_SHOW);

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

int64 FWindow::HandleMessage(uint32 Message, uint64 WParam, int64 LParam)
{
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

	case WM_SETFOCUS:
	case WM_KILLFOCUS:
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
		// Alt+F4 등 시스템 키 기본 동작은 유지
		return Message == WM_SYSKEYDOWN ? DefWindowProcW(Hwnd, Message, WParam, LParam) : 0;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		Event.Type = EWindowEventType::KeyUp;
		Event.Key  = TranslateKey(WParam, LParam);
		Dispatch(Event);
		return Message == WM_SYSKEYUP ? DefWindowProcW(Hwnd, Message, WParam, LParam) : 0;

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

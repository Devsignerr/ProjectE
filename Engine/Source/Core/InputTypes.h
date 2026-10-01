#pragma once

#include "Core/CoreTypes.h"

// 플랫폼 독립 키 코드
enum class EKey : uint16
{
	None = 0,

	A, B, C, D, E, F, G, H, I, J, K, L, M,
	N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

	Zero, One, Two, Three, Four, Five, Six, Seven, Eight, Nine,

	F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

	Escape, Tab, CapsLock, Space, Enter, Backspace,
	LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,

	Insert, Delete, Home, End, PageUp, PageDown,
	Left, Right, Up, Down,

	Numpad0, Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
	NumpadAdd, NumpadSubtract, NumpadMultiply, NumpadDivide, NumpadDecimal, NumpadEnter,

	Minus, Equals, LeftBracket, RightBracket, Backslash,
	Semicolon, Apostrophe, Comma, Period, Slash, Grave,

	Count
};

enum class EMouseButton : uint8
{
	Left = 0,
	Right,
	Middle,
	Thumb1,
	Thumb2,

	Count
};

// 게임패드 버튼 (XInput 배치 — Xbox 표기)
enum class EGamepadButton : uint8
{
	A = 0,
	B,
	X,
	Y,
	LeftShoulder,
	RightShoulder,
	Back,
	Start,
	LeftThumb,  // 왼쪽 스틱 누르기
	RightThumb,
	DPadUp,
	DPadDown,
	DPadLeft,
	DPadRight,

	Count
};

// 게임패드 아날로그 축. 스틱은 -1~1 (위/오른쪽이 +), 트리거는 0~1. 데드존은 적용하지 않은 원시 값 (액션 수정자 DeadZone이 처리)
enum class EGamepadAxis : uint8
{
	LeftStick = 0, // 2D (X, Y)
	RightStick,    // 2D
	LeftX,
	LeftY,
	RightX,
	RightY,
	LeftTrigger,
	RightTrigger,

	Count
};

// 게임패드 한 개의 이번 프레임 상태 (플랫폼 독립 — XInput 폴링 결과 또는 테스트/자동 검증 주입)
struct FGamepadState
{
	bool   bConnected   = false;
	uint16 Buttons      = 0; // 비트 = EGamepadButton
	float  LeftX        = 0.0f;
	float  LeftY        = 0.0f;
	float  RightX       = 0.0f;
	float  RightY       = 0.0f;
	float  LeftTrigger  = 0.0f;
	float  RightTrigger = 0.0f;

	bool IsButtonDown(EGamepadButton Button) const { return (Buttons >> static_cast<uint32>(Button)) & 1u; }
	void SetButton(EGamepadButton Button, bool bDown)
	{
		const uint16 Bit = static_cast<uint16>(1u << static_cast<uint32>(Button));
		Buttons          = bDown ? static_cast<uint16>(Buttons | Bit) : static_cast<uint16>(Buttons & ~Bit);
	}
};
static_assert(static_cast<size_t>(EGamepadButton::Count) <= 16, "게임패드 버튼 비트는 uint16");

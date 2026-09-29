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

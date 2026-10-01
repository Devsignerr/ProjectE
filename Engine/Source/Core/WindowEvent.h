#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputTypes.h"

enum class EWindowEventType : uint8
{
	Close,
	Resize,
	Focus,
	KeyDown,
	KeyUp,
	MouseMove,
	MouseButtonDown,
	MouseButtonUp,
	MouseWheel,
	Char, // 문자 입력 (WM_CHAR, IME 조합 완료 글자 포함)
	RawMouseMove, // 원시 마우스 이동 (WM_INPUT, 화면 가장자리/커서 잠금과 무관한 장치 이동량) — MouseX/MouseY = 델타
};

// 창에서 발생한 이벤트. 타입에 따라 관련 필드만 유효하다.
struct FWindowEvent
{
	EWindowEventType Type = EWindowEventType::Close;

	// Resize
	uint32 Width      = 0;
	uint32 Height     = 0;
	bool   bMinimized = false;

	// Focus
	bool bFocused = false;

	// KeyDown / KeyUp
	EKey Key     = EKey::None;
	bool bRepeat = false;

	// Mouse*
	EMouseButton Button     = EMouseButton::Left;
	int32        MouseX     = 0;
	int32        MouseY     = 0;
	float        WheelDelta = 0.0f; // 한 눈금 = 1.0

	// Char
	uint32 Character = 0; // 유니코드 코드 포인트 (UTF-16 서로게이트는 합쳐서 전달)
};

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
};

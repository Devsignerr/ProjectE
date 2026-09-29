#include "Core/Input.h"

void FInput::ProcessEvent(const FWindowEvent& Event)
{
	switch (Event.Type)
	{
	case EWindowEventType::KeyDown:
		if (Event.Key != EKey::None)
		{
			KeyStates[static_cast<size_t>(Event.Key)] = true;
		}
		break;

	case EWindowEventType::KeyUp:
		if (Event.Key != EKey::None)
		{
			KeyStates[static_cast<size_t>(Event.Key)] = false;
		}
		break;

	case EWindowEventType::MouseMove:
		MouseX = Event.MouseX;
		MouseY = Event.MouseY;
		break;

	case EWindowEventType::MouseButtonDown:
		ButtonStates[static_cast<size_t>(Event.Button)] = true;
		MouseX = Event.MouseX;
		MouseY = Event.MouseY;
		break;

	case EWindowEventType::MouseButtonUp:
		ButtonStates[static_cast<size_t>(Event.Button)] = false;
		MouseX = Event.MouseX;
		MouseY = Event.MouseY;
		break;

	case EWindowEventType::MouseWheel:
		WheelDelta += Event.WheelDelta;
		break;

	case EWindowEventType::Focus:
		if (!Event.bFocused)
		{
			ClearState();
		}
		break;

	default:
		break;
	}
}

void FInput::EndFrame()
{
	PrevKeyStates    = KeyStates;
	PrevButtonStates = ButtonStates;
	PrevMouseX       = MouseX;
	PrevMouseY       = MouseY;
	WheelDelta       = 0.0f;
}

void FInput::ClearState()
{
	KeyStates.reset();
	ButtonStates.reset();
}

bool FInput::IsKeyPressed(EKey Key) const
{
	const size_t Index = static_cast<size_t>(Key);
	return KeyStates[Index] && !PrevKeyStates[Index];
}

bool FInput::IsKeyReleased(EKey Key) const
{
	const size_t Index = static_cast<size_t>(Key);
	return !KeyStates[Index] && PrevKeyStates[Index];
}

bool FInput::IsMouseButtonPressed(EMouseButton Button) const
{
	const size_t Index = static_cast<size_t>(Button);
	return ButtonStates[Index] && !PrevButtonStates[Index];
}

bool FInput::IsMouseButtonReleased(EMouseButton Button) const
{
	const size_t Index = static_cast<size_t>(Button);
	return !ButtonStates[Index] && PrevButtonStates[Index];
}

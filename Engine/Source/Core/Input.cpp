#include "Core/Input.h"

void FInput::ProcessEvent(const FWindowEvent& Event)
{
	switch (Event.Type)
	{
	case EWindowEventType::KeyDown:
		if (Event.Key != EKey::None)
		{
			KeyStates[static_cast<size_t>(Event.Key)]    = true;
			RepeatStates[static_cast<size_t>(Event.Key)] = true;
		}
		break;

	case EWindowEventType::Char:
		TypedText.push_back(static_cast<char32_t>(Event.Character));
		break;

	case EWindowEventType::ImeComposition:
		CompositionText.assign(Event.Composition != nullptr ? Event.Composition : U"", Event.Composition != nullptr ? Event.CompositionLength : 0);
		CompositionCursor = Event.CompositionCursor;
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

	case EWindowEventType::RawMouseMove:
		LookDeltaX += static_cast<float>(Event.MouseX);
		LookDeltaY += static_cast<float>(Event.MouseY);
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
	LookDeltaX       = 0.0f;
	LookDeltaY       = 0.0f;
	RepeatStates.reset();
	TypedText.clear();
	PrevGamepad = Gamepad;
	for (FInputActionState& Action : Actions)
	{
		Action.bWasActive = Action.bActive;
	}
}

void FInput::SetState(const FKeyBits& Keys, const FButtonBits& Buttons, int32 InMouseX, int32 InMouseY, float Wheel)
{
	KeyStates    = Keys;
	ButtonStates = Buttons;
	MouseX       = InMouseX;
	MouseY       = InMouseY;
	WheelDelta  += Wheel;
}

void FInput::ClearState()
{
	KeyStates.reset();
	ButtonStates.reset();
	CompositionText.clear();
	CompositionCursor = 0;
}

bool FInput::IsKeyPressed(EKey Key) const
{
	const size_t Index = static_cast<size_t>(Key);
	return KeyStates[Index] && !PrevKeyStates[Index];
}

bool FInput::IsKeyRepeated(EKey Key) const
{
	return RepeatStates[static_cast<size_t>(Key)];
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

FInput FInput::WithoutMouseButtons() const
{
	FInput Copy = *this;
	Copy.ButtonStates.reset();
	Copy.PrevButtonStates.reset();
	Copy.WheelDelta = 0.0f;
	Copy.ReevaluateActions();
	return Copy;
}

FInput FInput::WithoutKeyboard() const
{
	FInput Copy = *this;
	Copy.KeyStates.reset();
	Copy.PrevKeyStates.reset();
	Copy.RepeatStates.reset();
	Copy.TypedText.clear();
	Copy.CompositionText.clear();
	Copy.CompositionCursor = 0;
	Copy.ReevaluateActions();
	return Copy;
}

FInput FInput::WithoutAnyInput() const
{
	FInput Copy = WithoutKeyboard();
	Copy.ButtonStates.reset();
	Copy.PrevButtonStates.reset();
	Copy.WheelDelta         = 0.0f;
	Copy.LookDeltaX         = 0.0f;
	Copy.LookDeltaY         = 0.0f;
	Copy.PrevMouseX         = Copy.MouseX;
	Copy.PrevMouseY         = Copy.MouseY;
	Copy.Gamepad            = FGamepadState{};
	Copy.Gamepad.bConnected = Gamepad.bConnected;
	Copy.PrevGamepad        = Copy.Gamepad;
	Copy.ReevaluateActions();
	// 받은 값(원격 입력)처럼 매핑이 없어 다시 계산하지 않는 경우도 비운다
	for (FInputActionState& Action : Copy.Actions)
	{
		Action.Value      = FVector2::ZeroVector;
		Action.bActive    = false;
		Action.bWasActive = false;
	}
	return Copy;
}

FVector2 FInput::ReadSource(const FInputSource& Source) const
{
	const auto Button = [](bool bDown) { return FVector2(bDown ? 1.0f : 0.0f, 0.0f); };
	switch (Source.Type)
	{
	case EInputSourceType::Key:
		return Source.Code < static_cast<uint16>(EKey::Count) ? Button(KeyStates[Source.Code]) : FVector2::ZeroVector;
	case EInputSourceType::MouseButton:
		return Source.Code < static_cast<uint16>(EMouseButton::Count) ? Button(ButtonStates[Source.Code]) : FVector2::ZeroVector;
	case EInputSourceType::MouseAxis:
		switch (static_cast<EMouseAxis>(Source.Code))
		{
		case EMouseAxis::XY:    return FVector2(LookDeltaX, LookDeltaY);
		case EMouseAxis::X:     return FVector2(LookDeltaX, 0.0f);
		case EMouseAxis::Y:     return FVector2(LookDeltaY, 0.0f);
		case EMouseAxis::Wheel: return FVector2(WheelDelta, 0.0f);
		default:                return FVector2::ZeroVector;
		}
	case EInputSourceType::GamepadButton:
		return Source.Code < static_cast<uint16>(EGamepadButton::Count) ? Button(Gamepad.IsButtonDown(static_cast<EGamepadButton>(Source.Code)))
		                                                                 : FVector2::ZeroVector;
	case EInputSourceType::GamepadAxis:
		switch (static_cast<EGamepadAxis>(Source.Code))
		{
		case EGamepadAxis::LeftStick:    return FVector2(Gamepad.LeftX, Gamepad.LeftY);
		case EGamepadAxis::RightStick:   return FVector2(Gamepad.RightX, Gamepad.RightY);
		case EGamepadAxis::LeftX:        return FVector2(Gamepad.LeftX, 0.0f);
		case EGamepadAxis::LeftY:        return FVector2(Gamepad.LeftY, 0.0f);
		case EGamepadAxis::RightX:       return FVector2(Gamepad.RightX, 0.0f);
		case EGamepadAxis::RightY:       return FVector2(Gamepad.RightY, 0.0f);
		case EGamepadAxis::LeftTrigger:  return FVector2(Gamepad.LeftTrigger, 0.0f);
		case EGamepadAxis::RightTrigger: return FVector2(Gamepad.RightTrigger, 0.0f);
		default:                         return FVector2::ZeroVector;
		}
	default:
		return FVector2::ZeroVector;
	}
}

namespace
{
	// 액션 목록을 매핑 순서에 맞춘다 (같은 이름의 이전 켜짐 상태는 유지 — 매핑을 고쳐도 눌림 판정이 튀지 않게)
	void SyncActionLayout(std::vector<FInputActionState>& Actions, const FInputMapping& Mapping)
	{
		bool bSame = Actions.size() == Mapping.Actions.size();
		for (size_t Index = 0; bSame && Index < Actions.size(); ++Index)
		{
			bSame = Actions[Index].Name == Mapping.Actions[Index].Name && Actions[Index].Type == Mapping.Actions[Index].Type;
		}
		if (bSame)
		{
			return;
		}
		std::vector<FInputActionState> Synced(Mapping.Actions.size());
		for (size_t Index = 0; Index < Synced.size(); ++Index)
		{
			Synced[Index].Name = Mapping.Actions[Index].Name;
			Synced[Index].Type = Mapping.Actions[Index].Type;
			for (const FInputActionState& Old : Actions)
			{
				if (Old.Name == Synced[Index].Name && Old.Type == Synced[Index].Type)
				{
					Synced[Index].bWasActive = Old.bWasActive;
				}
			}
		}
		Actions = std::move(Synced);
	}
} // namespace

void FInput::UpdateActions(const FInputMapping& Mapping, float DeltaSeconds)
{
	ActionMapping      = &Mapping;
	ActionDeltaSeconds = DeltaSeconds;
	ReevaluateActions();
}

void FInput::ReevaluateActions()
{
	if (ActionMapping == nullptr)
	{
		return;
	}
	SyncActionLayout(Actions, *ActionMapping);
	const auto Read = [this](const FInputSource& Source) { return ReadSource(Source); };
	for (size_t Index = 0; Index < Actions.size(); ++Index)
	{
		const InputActionMath::FResult Result = InputActionMath::Evaluate(ActionMapping->Actions[Index], Read, ActionDeltaSeconds);
		Actions[Index].Value                  = Result.Value;
		Actions[Index].bActive                = Result.bActive;
	}
}

void FInput::SetActionValues(const FInputMapping& Mapping, const std::vector<FInputActionState>& Values)
{
	ActionMapping = nullptr; // 받은 값이므로 다시 계산하지 않는다
	SyncActionLayout(Actions, Mapping);
	for (size_t Index = 0; Index < Actions.size() && Index < Values.size(); ++Index)
	{
		Actions[Index].Value   = Values[Index].Value;
		Actions[Index].bActive = Values[Index].bActive;
	}
}

const FInputActionState* FInput::FindAction(std::string_view Name) const
{
	for (const FInputActionState& Action : Actions)
	{
		if (Action.Name == Name)
		{
			return &Action;
		}
	}
	return nullptr;
}

FVector2 FInput::GetActionValue(std::string_view Name) const
{
	const FInputActionState* Action = FindAction(Name);
	return Action != nullptr ? Action->Value : FVector2::ZeroVector;
}

bool FInput::IsActionDown(std::string_view Name) const
{
	const FInputActionState* Action = FindAction(Name);
	return Action != nullptr && Action->bActive;
}

bool FInput::WasActionPressed(std::string_view Name) const
{
	const FInputActionState* Action = FindAction(Name);
	return Action != nullptr && Action->bActive && !Action->bWasActive;
}

bool FInput::WasActionReleased(std::string_view Name) const
{
	const FInputActionState* Action = FindAction(Name);
	return Action != nullptr && !Action->bActive && Action->bWasActive;
}

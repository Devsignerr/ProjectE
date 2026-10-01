#include "Core/GamepadInput.h"

#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"

#include <Xinput.h>

#include <algorithm>

namespace
{
	using FXInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

	float NormalizeThumb(SHORT Value)
	{
		return std::clamp(static_cast<float>(Value) / 32767.0f, -1.0f, 1.0f);
	}

	FGamepadState ConvertState(const XINPUT_GAMEPAD& Pad)
	{
		FGamepadState State;
		State.bConnected = true;
		const struct
		{
			WORD           Mask;
			EGamepadButton Button;
		} Buttons[] = {
			{ XINPUT_GAMEPAD_A, EGamepadButton::A },
			{ XINPUT_GAMEPAD_B, EGamepadButton::B },
			{ XINPUT_GAMEPAD_X, EGamepadButton::X },
			{ XINPUT_GAMEPAD_Y, EGamepadButton::Y },
			{ XINPUT_GAMEPAD_LEFT_SHOULDER, EGamepadButton::LeftShoulder },
			{ XINPUT_GAMEPAD_RIGHT_SHOULDER, EGamepadButton::RightShoulder },
			{ XINPUT_GAMEPAD_BACK, EGamepadButton::Back },
			{ XINPUT_GAMEPAD_START, EGamepadButton::Start },
			{ XINPUT_GAMEPAD_LEFT_THUMB, EGamepadButton::LeftThumb },
			{ XINPUT_GAMEPAD_RIGHT_THUMB, EGamepadButton::RightThumb },
			{ XINPUT_GAMEPAD_DPAD_UP, EGamepadButton::DPadUp },
			{ XINPUT_GAMEPAD_DPAD_DOWN, EGamepadButton::DPadDown },
			{ XINPUT_GAMEPAD_DPAD_LEFT, EGamepadButton::DPadLeft },
			{ XINPUT_GAMEPAD_DPAD_RIGHT, EGamepadButton::DPadRight },
		};
		for (const auto& Entry : Buttons)
		{
			State.SetButton(Entry.Button, (Pad.wButtons & Entry.Mask) != 0);
		}
		State.LeftX        = NormalizeThumb(Pad.sThumbLX);
		State.LeftY        = NormalizeThumb(Pad.sThumbLY);
		State.RightX       = NormalizeThumb(Pad.sThumbRX);
		State.RightY       = NormalizeThumb(Pad.sThumbRY);
		State.LeftTrigger  = static_cast<float>(Pad.bLeftTrigger) / 255.0f;
		State.RightTrigger = static_cast<float>(Pad.bRightTrigger) / 255.0f;
		return State;
	}
} // namespace

FGamepadInput::FGamepadInput()
{
	for (const wchar_t* Name : { L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll" })
	{
		if (HMODULE Loaded = LoadLibraryW(Name))
		{
			if (FARPROC Function = GetProcAddress(Loaded, "XInputGetState"))
			{
				Module           = Loaded;
				GetStateFunction = reinterpret_cast<void*>(Function);
				return;
			}
			FreeLibrary(Loaded);
		}
	}
	E_LOG(LogCore, Warning, "XInput을 불러오지 못해 게임패드를 쓰지 않습니다");
}

FGamepadInput::~FGamepadInput()
{
	if (Module != nullptr)
	{
		FreeLibrary(static_cast<HMODULE>(Module));
	}
}

void FGamepadInput::Poll(float DeltaSeconds)
{
	if (GetStateFunction == nullptr)
	{
		return;
	}
	const FXInputGetState GetState = reinterpret_cast<FXInputGetState>(GetStateFunction);
	for (uint32 Index = 0; Index < MaxGamepads; ++Index)
	{
		if (!States[Index].bConnected)
		{
			RetrySeconds[Index] -= DeltaSeconds;
			if (RetrySeconds[Index] > 0.0f)
			{
				continue;
			}
			RetrySeconds[Index] = ReconnectIntervalSeconds;
		}
		XINPUT_STATE Raw   = {};
		const bool   bWas  = States[Index].bConnected;
		if (GetState(Index, &Raw) == ERROR_SUCCESS)
		{
			States[Index] = ConvertState(Raw.Gamepad);
		}
		else
		{
			States[Index] = FGamepadState();
		}
		if (bWas != States[Index].bConnected)
		{
			E_LOG(LogCore, Display, "게임패드 {} {}", Index, States[Index].bConnected ? "연결됨" : "연결 해제됨");
		}
	}
}

int32 FGamepadInput::GetPrimaryIndex() const
{
	for (uint32 Index = 0; Index < MaxGamepads; ++Index)
	{
		if (States[Index].bConnected)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

const FGamepadState& FGamepadInput::GetPrimary() const
{
	static const FGamepadState Disconnected;
	const int32                Index = GetPrimaryIndex();
	return Index >= 0 ? States[static_cast<size_t>(Index)] : Disconnected;
}

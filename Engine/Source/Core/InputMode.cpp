#include "Core/InputMode.h"

#include "Core/Input.h"

#include <cctype>

namespace
{
	struct FInputModeGlobal
	{
		EInputMode Mode     = EInputMode::GameAndUI;
		uint32     Revision = 0;
	};

	// 엔진 DLL 안 하나 (.cpp 함수 지역 static — 바이너리마다 따로 생기지 않는다)
	FInputModeGlobal& GetGlobal()
	{
		static FInputModeGlobal Global;
		return Global;
	}

	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		if (A.size() != B.size())
		{
			return false;
		}
		for (size_t Index = 0; Index < A.size(); ++Index)
		{
			if (std::tolower(static_cast<unsigned char>(A[Index])) != std::tolower(static_cast<unsigned char>(B[Index])))
			{
				return false;
			}
		}
		return true;
	}
}

const char* ToString(EInputMode Mode)
{
	switch (Mode)
	{
	case EInputMode::GameOnly: return "GameOnly";
	case EInputMode::UIOnly:   return "UIOnly";
	default:                   return "GameAndUI";
	}
}

bool TryParseInputMode(std::string_view Name, EInputMode& OutMode)
{
	for (const EInputMode Mode : { EInputMode::GameOnly, EInputMode::GameAndUI, EInputMode::UIOnly })
	{
		if (EqualsIgnoreCase(Name, ToString(Mode)))
		{
			OutMode = Mode;
			return true;
		}
	}
	return false;
}

FInputModeRouting GetInputModeRouting(EInputMode Mode)
{
	FInputModeRouting Routing;
	switch (Mode)
	{
	case EInputMode::GameOnly:
		Routing.bUIInput    = false;
		Routing.bLockCursor = true;
		break;
	case EInputMode::UIOnly:
		Routing.bGameInput = false;
		break;
	default:
		break;
	}
	return Routing;
}

const FInput& SelectGameInput(EInputMode Mode, const FInput& Raw, bool bUIPointer, bool bUIKeyboard, FInput& Storage)
{
	const FInputModeRouting Routing = GetInputModeRouting(Mode);
	if (!Routing.bGameInput)
	{
		Storage = Raw.WithoutAnyInput();
		return Storage;
	}
	// GameOnly는 UI에 입력을 주지 않으므로 bUIPointer/bUIKeyboard가 거짓이다 (콘솔 키보드는 예외로 여기서 뺀다)
	if (!bUIPointer && !bUIKeyboard)
	{
		return Raw;
	}
	Storage = bUIPointer ? Raw.WithoutMouseButtons() : Raw;
	if (bUIKeyboard)
	{
		Storage = Storage.WithoutKeyboard();
	}
	return Storage;
}

EInputMode FInputModeState::Get()
{
	return GetGlobal().Mode;
}

void FInputModeState::Set(EInputMode Mode)
{
	FInputModeGlobal& Global = GetGlobal();
	Global.Mode              = Mode;
	++Global.Revision;
}

void FInputModeState::Reset()
{
	Set(EInputMode::GameAndUI);
}

uint32 FInputModeState::GetRevision()
{
	return GetGlobal().Revision;
}

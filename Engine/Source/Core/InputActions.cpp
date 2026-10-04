#include "Core/InputActions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

namespace
{
	// EKey 순서와 1:1
	constexpr std::array<const char*, static_cast<size_t>(EKey::Count)> GKeyNames = {
		"None",
		"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
		"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
		"Escape", "Tab", "CapsLock", "Space", "Enter", "Backspace",
		"LeftShift", "RightShift", "LeftControl", "RightControl", "LeftAlt", "RightAlt",
		"Insert", "Delete", "Home", "End", "PageUp", "PageDown",
		"Left", "Right", "Up", "Down",
		"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
		"NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide", "NumpadDecimal", "NumpadEnter",
		"Minus", "Equals", "LeftBracket", "RightBracket", "Backslash",
		"Semicolon", "Apostrophe", "Comma", "Period", "Slash", "Grave",
	};

	constexpr std::array<const char*, static_cast<size_t>(EMouseButton::Count)> GMouseButtonNames = {
		"MouseLeft", "MouseRight", "MouseMiddle", "MouseThumb1", "MouseThumb2",
	};

	constexpr std::array<const char*, static_cast<size_t>(EMouseAxis::Count)> GMouseAxisNames = {
		"MouseXY", "MouseX", "MouseY", "MouseWheel",
	};

	constexpr std::array<const char*, static_cast<size_t>(EGamepadButton::Count)> GGamepadButtonNames = {
		"A", "B", "X", "Y", "LeftShoulder", "RightShoulder", "Back", "Start", "LeftThumb", "RightThumb",
		"DPadUp", "DPadDown", "DPadLeft", "DPadRight",
	};

	constexpr std::array<const char*, static_cast<size_t>(EGamepadAxis::Count)> GGamepadAxisNames = {
		"LeftStick", "RightStick", "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger",
	};

	constexpr std::string_view GGamepadPrefix = "Gamepad_";

	template <size_t N>
	int32 FindName(const std::array<const char*, N>& Names, std::string_view Name, size_t First = 0)
	{
		for (size_t Index = First; Index < N; ++Index)
		{
			if (Name == Names[Index])
			{
				return static_cast<int32>(Index);
			}
		}
		return -1;
	}

	void AppendError(std::string* Error, const std::string& Message)
	{
		if (Error != nullptr)
		{
			if (!Error->empty())
			{
				*Error += "; ";
			}
			*Error += Message;
		}
	}

	// float를 JSON에 쓸 때 0.1f → 0.10000000149011612처럼 길어지지 않게 (소수 6자리 반올림한 double)
	double RoundForJson(float Value)
	{
		return std::round(static_cast<double>(Value) * 1.0e6) / 1.0e6;
	}

	nlohmann::ordered_json ModifierToJson(const FInputModifier& Modifier)
	{
		nlohmann::ordered_json Json;
		Json["Type"] = InputNames::ToString(Modifier.Type);
		switch (Modifier.Type)
		{
		case EInputModifierType::Negate:
			Json["X"] = Modifier.bX;
			Json["Y"] = Modifier.bY;
			break;
		case EInputModifierType::DeadZone:
			Json["Lower"]  = RoundForJson(Modifier.Lower);
			Json["Upper"]  = RoundForJson(Modifier.Upper);
			Json["Radial"] = Modifier.bRadial;
			break;
		case EInputModifierType::Scale:
			Json["X"] = RoundForJson(Modifier.Scale.X);
			Json["Y"] = RoundForJson(Modifier.Scale.Y);
			break;
		default:
			break;
		}
		return Json;
	}

	bool ModifierFromJson(const nlohmann::json& Json, FInputModifier& Out, std::string* Error)
	{
		EInputModifierType Type = EInputModifierType::Negate;
		if (!Json.is_object() || !InputNames::TryParseModifierType(Json.value("Type", std::string()), Type))
		{
			AppendError(Error, std::format("알 수 없는 수정자 {}", Json.dump()));
			return false;
		}
		Out      = FInputModifier();
		Out.Type = Type;
		switch (Type)
		{
		case EInputModifierType::Negate:
			Out.bX = Json.value("X", true);
			Out.bY = Json.value("Y", true);
			break;
		case EInputModifierType::DeadZone:
			Out.Lower   = Json.value("Lower", 0.2f);
			Out.Upper   = Json.value("Upper", 1.0f);
			Out.bRadial = Json.value("Radial", true);
			break;
		case EInputModifierType::Scale:
			Out.Scale = FVector2(Json.value("X", 1.0f), Json.value("Y", 1.0f));
			break;
		default:
			break;
		}
		return true;
	}

	// 0~1로 다시 펼친 크기
	float RemapDeadZone(float Magnitude, float Lower, float Upper)
	{
		if (Magnitude <= Lower)
		{
			return 0.0f;
		}
		if (Upper <= Lower)
		{
			return 1.0f;
		}
		return std::clamp((Magnitude - Lower) / (Upper - Lower), 0.0f, 1.0f);
	}
} // namespace

// ---- FInputSource / FInputModifier

bool FInputSource::Is2D() const
{
	if (Type == EInputSourceType::MouseAxis)
	{
		return Code == static_cast<uint16>(EMouseAxis::XY);
	}
	if (Type == EInputSourceType::GamepadAxis)
	{
		return Code == static_cast<uint16>(EGamepadAxis::LeftStick) || Code == static_cast<uint16>(EGamepadAxis::RightStick);
	}
	return false;
}

FInputModifier FInputModifier::MakeNegate(bool bInX, bool bInY)
{
	FInputModifier Modifier;
	Modifier.Type = EInputModifierType::Negate;
	Modifier.bX   = bInX;
	Modifier.bY   = bInY;
	return Modifier;
}

FInputModifier FInputModifier::MakeSwizzle()
{
	FInputModifier Modifier;
	Modifier.Type = EInputModifierType::Swizzle;
	return Modifier;
}

FInputModifier FInputModifier::MakeDeadZone(float InLower, float InUpper, bool bInRadial)
{
	FInputModifier Modifier;
	Modifier.Type    = EInputModifierType::DeadZone;
	Modifier.Lower   = InLower;
	Modifier.Upper   = InUpper;
	Modifier.bRadial = bInRadial;
	return Modifier;
}

FInputModifier FInputModifier::MakeScale(FVector2 InScale)
{
	FInputModifier Modifier;
	Modifier.Type  = EInputModifierType::Scale;
	Modifier.Scale = InScale;
	return Modifier;
}

FInputModifier FInputModifier::MakeScaleByDeltaTime()
{
	FInputModifier Modifier;
	Modifier.Type = EInputModifierType::ScaleByDeltaTime;
	return Modifier;
}

// ---- FInputMapping

const FInputAction* FInputMapping::Find(std::string_view Name) const
{
	const auto Found = std::find_if(Actions.begin(), Actions.end(), [Name](const FInputAction& Action) { return Action.Name == Name; });
	return Found != Actions.end() ? &*Found : nullptr;
}

FInputAction* FInputMapping::Find(std::string_view Name)
{
	return const_cast<FInputAction*>(static_cast<const FInputMapping*>(this)->Find(Name));
}

bool FInputMapping::IsSourceBound(const FInputSource& Source) const
{
	for (const FInputAction& Action : Actions)
	{
		for (const FInputBinding& Binding : Action.Bindings)
		{
			if (Binding.Source == Source)
			{
				return true;
			}
		}
	}
	return false;
}

uint32 FInputMapping::GetLayoutHash() const
{
	uint32     Hash   = 2166136261u; // FNV-1a
	const auto Mix    = [&Hash](uint8 Byte) { Hash = (Hash ^ Byte) * 16777619u; };
	for (const FInputAction& Action : Actions)
	{
		for (const char Char : Action.Name)
		{
			Mix(static_cast<uint8>(Char));
		}
		Mix(0);
		Mix(static_cast<uint8>(Action.Type));
	}
	return Hash;
}

bool FInputMapping::FromJson(std::string_view Json, FInputMapping& OutMapping, std::string* Error)
{
	const nlohmann::json Root = nlohmann::json::parse(Json, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Root.is_discarded() || !Root.is_object())
	{
		AppendError(Error, "JSON 형식 오류");
		return false;
	}
	FInputMapping Result;
	const auto    ActionsIt = Root.find("Actions");
	if (ActionsIt != Root.end() && ActionsIt->is_array())
	{
		for (const nlohmann::json& ActionJson : *ActionsIt)
		{
			if (!ActionJson.is_object())
			{
				continue;
			}
			FInputAction Action;
			Action.Name = ActionJson.value("Name", std::string());
			if (Action.Name.empty() || Result.Find(Action.Name) != nullptr)
			{
				AppendError(Error, std::format("이름이 없거나 겹치는 액션 '{}'", Action.Name));
				continue;
			}
			if (!InputNames::TryParseActionType(ActionJson.value("Type", std::string("Button")), Action.Type))
			{
				AppendError(Error, std::format("액션 '{}'의 종류를 모릅니다", Action.Name));
			}
			Action.Description        = ActionJson.value("Description", std::string());
			Action.ActuationThreshold = ActionJson.value("ActuationThreshold", 0.5f);
			const auto BindingsIt     = ActionJson.find("Bindings");
			if (BindingsIt != ActionJson.end() && BindingsIt->is_array())
			{
				for (const nlohmann::json& BindingJson : *BindingsIt)
				{
					FInputBinding Binding;
					if (!BindingJson.is_object() || !InputNames::TryParseSource(BindingJson.value("Source", std::string()), Binding.Source))
					{
						AppendError(Error, std::format("액션 '{}': 알 수 없는 입력 {}", Action.Name, BindingJson.dump()));
						continue;
					}
					const auto ModifiersIt = BindingJson.find("Modifiers");
					if (ModifiersIt != BindingJson.end() && ModifiersIt->is_array())
					{
						for (const nlohmann::json& ModifierJson : *ModifiersIt)
						{
							FInputModifier Modifier;
							if (ModifierFromJson(ModifierJson, Modifier, Error))
							{
								Binding.Modifiers.push_back(Modifier);
							}
						}
					}
					Action.Bindings.push_back(std::move(Binding));
				}
			}
			Result.Actions.push_back(std::move(Action));
		}
	}
	OutMapping = std::move(Result);
	return true;
}

std::string FInputMapping::ToJson() const
{
	nlohmann::ordered_json Root;
	Root["Version"] = 1;
	Root["Actions"] = nlohmann::ordered_json::array();
	for (const FInputAction& Action : Actions)
	{
		nlohmann::ordered_json ActionJson;
		ActionJson["Name"] = Action.Name;
		ActionJson["Type"] = InputNames::ToString(Action.Type);
		if (!Action.Description.empty())
		{
			ActionJson["Description"] = Action.Description;
		}
		ActionJson["ActuationThreshold"] = RoundForJson(Action.ActuationThreshold);
		ActionJson["Bindings"]           = nlohmann::ordered_json::array();
		for (const FInputBinding& Binding : Action.Bindings)
		{
			nlohmann::ordered_json BindingJson;
			BindingJson["Source"] = InputNames::ToString(Binding.Source);
			if (!Binding.Modifiers.empty())
			{
				BindingJson["Modifiers"] = nlohmann::ordered_json::array();
				for (const FInputModifier& Modifier : Binding.Modifiers)
				{
					BindingJson["Modifiers"].push_back(ModifierToJson(Modifier));
				}
			}
			ActionJson["Bindings"].push_back(std::move(BindingJson));
		}
		Root["Actions"].push_back(std::move(ActionJson));
	}
	return Root.dump(1, '\t') + "\n";
}

// ---- InputNames

const char* InputNames::GetKeyName(EKey Key)
{
	const size_t Index = static_cast<size_t>(Key);
	return Index < GKeyNames.size() ? GKeyNames[Index] : "None";
}

bool InputNames::TryParseKey(std::string_view Name, EKey& OutKey)
{
	const int32 Index = FindName(GKeyNames, Name, 1);
	if (Index < 0)
	{
		return false;
	}
	OutKey = static_cast<EKey>(Index);
	return true;
}

const char* InputNames::GetGamepadButtonName(EGamepadButton Button)
{
	const size_t Index = static_cast<size_t>(Button);
	return Index < GGamepadButtonNames.size() ? GGamepadButtonNames[Index] : "";
}

bool InputNames::TryParseGamepadButton(std::string_view Name, EGamepadButton& OutButton)
{
	const int32 Index = FindName(GGamepadButtonNames, Name);
	if (Index < 0)
	{
		return false;
	}
	OutButton = static_cast<EGamepadButton>(Index);
	return true;
}

const char* InputNames::GetGamepadAxisName(EGamepadAxis Axis)
{
	const size_t Index = static_cast<size_t>(Axis);
	return Index < GGamepadAxisNames.size() ? GGamepadAxisNames[Index] : "";
}

bool InputNames::TryParseGamepadAxis(std::string_view Name, EGamepadAxis& OutAxis)
{
	const int32 Index = FindName(GGamepadAxisNames, Name);
	if (Index < 0)
	{
		return false;
	}
	OutAxis = static_cast<EGamepadAxis>(Index);
	return true;
}

std::string InputNames::ToString(const FInputSource& Source)
{
	switch (Source.Type)
	{
	case EInputSourceType::Key:
		return GetKeyName(static_cast<EKey>(Source.Code));
	case EInputSourceType::MouseButton:
		return Source.Code < GMouseButtonNames.size() ? GMouseButtonNames[Source.Code] : "None";
	case EInputSourceType::MouseAxis:
		return Source.Code < GMouseAxisNames.size() ? GMouseAxisNames[Source.Code] : "None";
	case EInputSourceType::GamepadButton:
		return std::string(GGamepadPrefix) + GetGamepadButtonName(static_cast<EGamepadButton>(Source.Code));
	case EInputSourceType::GamepadAxis:
		return std::string(GGamepadPrefix) + GetGamepadAxisName(static_cast<EGamepadAxis>(Source.Code));
	default:
		return "None";
	}
}

bool InputNames::TryParseSource(std::string_view Name, FInputSource& OutSource)
{
	if (Name.starts_with(GGamepadPrefix))
	{
		const std::string_view Rest = Name.substr(GGamepadPrefix.size());
		EGamepadButton         Button;
		if (TryParseGamepadButton(Rest, Button))
		{
			OutSource = FInputSource::Gamepad(Button);
			return true;
		}
		EGamepadAxis Axis;
		if (TryParseGamepadAxis(Rest, Axis))
		{
			OutSource = FInputSource::Gamepad(Axis);
			return true;
		}
		return false;
	}
	if (const int32 Index = FindName(GMouseButtonNames, Name); Index >= 0)
	{
		OutSource = FInputSource::Mouse(static_cast<EMouseButton>(Index));
		return true;
	}
	if (const int32 Index = FindName(GMouseAxisNames, Name); Index >= 0)
	{
		OutSource = FInputSource::Mouse(static_cast<EMouseAxis>(Index));
		return true;
	}
	EKey Key;
	if (TryParseKey(Name, Key))
	{
		OutSource = FInputSource::Key(Key);
		return true;
	}
	return false;
}

const std::vector<std::string>& InputNames::GetAllSourceNames()
{
	static const std::vector<std::string> Names = [] {
		std::vector<std::string> Result;
		for (size_t Index = 1; Index < GKeyNames.size(); ++Index)
		{
			Result.emplace_back(GKeyNames[Index]);
		}
		Result.insert(Result.end(), GMouseButtonNames.begin(), GMouseButtonNames.end());
		Result.insert(Result.end(), GMouseAxisNames.begin(), GMouseAxisNames.end());
		for (const char* Button : GGamepadButtonNames)
		{
			Result.push_back(std::string(GGamepadPrefix) + Button);
		}
		for (const char* Axis : GGamepadAxisNames)
		{
			Result.push_back(std::string(GGamepadPrefix) + Axis);
		}
		return Result;
	}();
	return Names;
}

const char* InputNames::ToString(EInputActionType Type)
{
	switch (Type)
	{
	case EInputActionType::Button: return "Button";
	case EInputActionType::Axis1D: return "Axis1D";
	case EInputActionType::Axis2D: return "Axis2D";
	}
	return "Button";
}

bool InputNames::TryParseActionType(std::string_view Name, EInputActionType& OutType)
{
	for (const EInputActionType Type : { EInputActionType::Button, EInputActionType::Axis1D, EInputActionType::Axis2D })
	{
		if (Name == ToString(Type))
		{
			OutType = Type;
			return true;
		}
	}
	return false;
}

const char* InputNames::ToString(EInputModifierType Type)
{
	switch (Type)
	{
	case EInputModifierType::Negate:           return "Negate";
	case EInputModifierType::Swizzle:          return "Swizzle";
	case EInputModifierType::DeadZone:         return "DeadZone";
	case EInputModifierType::Scale:            return "Scale";
	case EInputModifierType::ScaleByDeltaTime: return "ScaleByDeltaTime";
	}
	return "Negate";
}

bool InputNames::TryParseModifierType(std::string_view Name, EInputModifierType& OutType)
{
	for (const EInputModifierType Type : { EInputModifierType::Negate, EInputModifierType::Swizzle, EInputModifierType::DeadZone, EInputModifierType::Scale,
	                                       EInputModifierType::ScaleByDeltaTime })
	{
		if (Name == ToString(Type))
		{
			OutType = Type;
			return true;
		}
	}
	return false;
}

// ---- InputActionMath

FVector2 InputActionMath::ApplyModifier(const FInputModifier& Modifier, FVector2 Value, float DeltaSeconds)
{
	switch (Modifier.Type)
	{
	case EInputModifierType::Negate:
		return FVector2(Modifier.bX ? -Value.X : Value.X, Modifier.bY ? -Value.Y : Value.Y);
	case EInputModifierType::Swizzle:
		return FVector2(Value.Y, Value.X);
	case EInputModifierType::DeadZone:
		if (Modifier.bRadial)
		{
			const float Length = Value.Length();
			return Length > 0.0f ? Value * (RemapDeadZone(Length, Modifier.Lower, Modifier.Upper) / Length) : FVector2::ZeroVector;
		}
		return FVector2(std::copysign(RemapDeadZone(std::abs(Value.X), Modifier.Lower, Modifier.Upper), Value.X),
		                std::copysign(RemapDeadZone(std::abs(Value.Y), Modifier.Lower, Modifier.Upper), Value.Y));
	case EInputModifierType::Scale:
		return Value * Modifier.Scale;
	case EInputModifierType::ScaleByDeltaTime:
		return Value * DeltaSeconds;
	}
	return Value;
}

FVector2 InputActionMath::ApplyModifiers(const std::vector<FInputModifier>& Modifiers, FVector2 Value, float DeltaSeconds)
{
	for (const FInputModifier& Modifier : Modifiers)
	{
		Value = ApplyModifier(Modifier, Value, DeltaSeconds);
	}
	return Value;
}

float InputActionMath::GetMagnitude(EInputActionType Type, FVector2 Value)
{
	return Type == EInputActionType::Axis2D ? Value.Length() : std::abs(Value.X);
}

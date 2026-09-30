#include "AI/BehaviorTree/BehaviorTreeTypes.h"

#include <charconv>
#include <cctype>

namespace
{
	std::string_view Trim(std::string_view Text)
	{
		while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
		{
			Text.remove_prefix(1);
		}
		while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
		{
			Text.remove_suffix(1);
		}
		return Text;
	}

	bool ParseInt(std::string_view Text, int32& Out)
	{
		Text = Trim(Text);
		const auto [Ptr, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Out);
		return Error == std::errc() && Ptr == Text.data() + Text.size() && !Text.empty();
	}

	bool ParseFloat(std::string_view Text, float& Out)
	{
		Text = Trim(Text);
		const auto [Ptr, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Out);
		return Error == std::errc() && Ptr == Text.data() + Text.size() && !Text.empty();
	}
} // namespace

const FBTParamValue* FBTNodeParams::Find(std::string_view Name) const
{
	for (const FBTParam& Param : Values)
	{
		if (Param.Name == Name)
		{
			return &Param.Value;
		}
	}
	return nullptr;
}

bool FBTNodeParams::GetBool(std::string_view Name, bool Default) const
{
	const FBTParamValue* Value = Find(Name);
	return Value && std::holds_alternative<bool>(*Value) ? std::get<bool>(*Value) : Default;
}

int32 FBTNodeParams::GetInt(std::string_view Name, int32 Default) const
{
	const FBTParamValue* Value = Find(Name);
	if (!Value)
	{
		return Default;
	}
	if (const int32* Int = std::get_if<int32>(Value))
	{
		return *Int;
	}
	if (const float* Float = std::get_if<float>(Value))
	{
		return static_cast<int32>(*Float);
	}
	return Default;
}

float FBTNodeParams::GetFloat(std::string_view Name, float Default) const
{
	const FBTParamValue* Value = Find(Name);
	if (!Value)
	{
		return Default;
	}
	if (const float* Float = std::get_if<float>(Value))
	{
		return *Float;
	}
	if (const int32* Int = std::get_if<int32>(Value))
	{
		return static_cast<float>(*Int);
	}
	return Default;
}

std::string FBTNodeParams::GetString(std::string_view Name, std::string_view Default) const
{
	const FBTParamValue* Value = Find(Name);
	return Value && std::holds_alternative<std::string>(*Value) ? std::get<std::string>(*Value) : std::string(Default);
}

FVector3 FBTNodeParams::GetVector(std::string_view Name, const FVector3& Default) const
{
	const FBTParamValue* Value = Find(Name);
	return Value && std::holds_alternative<FVector3>(*Value) ? std::get<FVector3>(*Value) : Default;
}

namespace BehaviorTreeTypes
{
	const char* ToString(EBTStatus Status)
	{
		switch (Status)
		{
		case EBTStatus::Running: return "Running";
		case EBTStatus::Success: return "Success";
		case EBTStatus::Failure: return "Failure";
		}
		return "Unknown";
	}

	const char* ToString(EBTNodeCategory Category)
	{
		switch (Category)
		{
		case EBTNodeCategory::Composite: return "Composite";
		case EBTNodeCategory::Task:      return "Task";
		case EBTNodeCategory::Decorator: return "Decorator";
		case EBTNodeCategory::Service:   return "Service";
		}
		return "Unknown";
	}

	const char* ToString(EBTAbortMode Mode)
	{
		switch (Mode)
		{
		case EBTAbortMode::None:          return "None";
		case EBTAbortMode::Self:          return "Self";
		case EBTAbortMode::LowerPriority: return "LowerPriority";
		case EBTAbortMode::Both:          return "Both";
		}
		return "Unknown";
	}

	const char* ToString(EBlackboardKeyType Type)
	{
		switch (Type)
		{
		case EBlackboardKeyType::Bool:   return "Bool";
		case EBlackboardKeyType::Int:    return "Int";
		case EBlackboardKeyType::Float:  return "Float";
		case EBlackboardKeyType::Vector: return "Vector";
		case EBlackboardKeyType::Entity: return "Entity";
		case EBlackboardKeyType::String: return "String";
		}
		return "Unknown";
	}

	std::optional<EBTNodeCategory> ParseNodeCategory(std::string_view Text)
	{
		for (EBTNodeCategory Category : { EBTNodeCategory::Composite, EBTNodeCategory::Task, EBTNodeCategory::Decorator, EBTNodeCategory::Service })
		{
			if (Text == ToString(Category))
			{
				return Category;
			}
		}
		return std::nullopt;
	}

	std::optional<EBTAbortMode> ParseAbortMode(std::string_view Text)
	{
		for (EBTAbortMode Mode : { EBTAbortMode::None, EBTAbortMode::Self, EBTAbortMode::LowerPriority, EBTAbortMode::Both })
		{
			if (Text == ToString(Mode))
			{
				return Mode;
			}
		}
		return std::nullopt;
	}

	std::optional<EBlackboardKeyType> ParseBlackboardKeyType(std::string_view Text)
	{
		for (EBlackboardKeyType Type : { EBlackboardKeyType::Bool, EBlackboardKeyType::Int, EBlackboardKeyType::Float,
		                                 EBlackboardKeyType::Vector, EBlackboardKeyType::Entity, EBlackboardKeyType::String })
		{
			if (Text == ToString(Type))
			{
				return Type;
			}
		}
		return std::nullopt;
	}

	EPropertyType GetParamType(const FBTParamValue& Value)
	{
		switch (Value.index())
		{
		case 0:  return EPropertyType::Bool;
		case 1:  return EPropertyType::Int32;
		case 2:  return EPropertyType::Float;
		case 3:  return EPropertyType::String;
		default: return EPropertyType::Vector3;
		}
	}

	std::optional<FBTParamValue> MakeDefaultParam(EPropertyType Type)
	{
		switch (Type)
		{
		case EPropertyType::Bool:    return FBTParamValue(false);
		case EPropertyType::Int32:   return FBTParamValue(int32(0));
		case EPropertyType::Float:   return FBTParamValue(0.0f);
		case EPropertyType::String:  return FBTParamValue(std::string());
		case EPropertyType::Vector3: return FBTParamValue(FVector3::ZeroVector);
		default:                     return std::nullopt;
		}
	}

	std::optional<FBTParamValue> CoerceParam(const FBTParamValue& Value, EPropertyType Type)
	{
		if (GetParamType(Value) == Type)
		{
			return Value;
		}
		if (Type == EPropertyType::Float)
		{
			if (const int32* Int = std::get_if<int32>(&Value))
			{
				return FBTParamValue(static_cast<float>(*Int));
			}
		}
		if (Type == EPropertyType::Int32)
		{
			if (const float* Float = std::get_if<float>(&Value))
			{
				return FBTParamValue(static_cast<int32>(*Float));
			}
		}
		return std::nullopt;
	}

	bool MatchesKeyType(const FBlackboardValue& Value, EBlackboardKeyType Type)
	{
		return Value.index() == static_cast<size_t>(Type);
	}

	bool ParseBlackboardValue(std::string_view Text, EBlackboardKeyType Type, FBlackboardValue& OutValue)
	{
		switch (Type)
		{
		case EBlackboardKeyType::Bool:
		{
			const std::string_view Trimmed = Trim(Text);
			if (Trimmed == "true" || Trimmed == "True" || Trimmed == "1")
			{
				OutValue = true;
				return true;
			}
			if (Trimmed == "false" || Trimmed == "False" || Trimmed == "0")
			{
				OutValue = false;
				return true;
			}
			return false;
		}
		case EBlackboardKeyType::Int:
		{
			int32 Value = 0;
			if (!ParseInt(Text, Value))
			{
				return false;
			}
			OutValue = Value;
			return true;
		}
		case EBlackboardKeyType::Float:
		{
			float Value = 0.0f;
			if (!ParseFloat(Text, Value))
			{
				return false;
			}
			OutValue = Value;
			return true;
		}
		case EBlackboardKeyType::Vector:
		{
			float  Components[3] = {};
			size_t Start         = 0;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const size_t Comma = Text.find(',', Start);
				if ((Index < 2) == (Comma == std::string_view::npos))
				{
					return false;
				}
				const std::string_view Part = Text.substr(Start, Index < 2 ? Comma - Start : std::string_view::npos);
				if (!ParseFloat(Part, Components[Index]))
				{
					return false;
				}
				Start = Comma + 1;
			}
			OutValue = FVector3(Components[0], Components[1], Components[2]);
			return true;
		}
		case EBlackboardKeyType::String:
			OutValue = std::string(Text);
			return true;
		case EBlackboardKeyType::Entity:
			return false;
		}
		return false;
	}
} // namespace BehaviorTreeTypes

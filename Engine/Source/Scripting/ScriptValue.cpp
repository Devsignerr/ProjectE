#include "Scripting/ScriptValue.h"

#include "Core/Log.h"

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

E_DECLARE_LOG_CATEGORY(LogScript)

FScriptValue FScriptValue::MakeBool(bool bValue)
{
	FScriptValue Value;
	Value.Type  = EScriptValueType::Bool;
	Value.bBool = bValue;
	return Value;
}

FScriptValue FScriptValue::MakeNumber(double Number, bool bIsInteger)
{
	FScriptValue Value;
	Value.Type     = EScriptValueType::Number;
	Value.Number   = Number;
	Value.bInteger = bIsInteger;
	return Value;
}

FScriptValue FScriptValue::MakeString(std::string String)
{
	FScriptValue Value;
	Value.Type   = EScriptValueType::String;
	Value.String = std::move(String);
	return Value;
}

FScriptValue FScriptValue::MakeVector3(const FVector3& Vector)
{
	FScriptValue Value;
	Value.Type   = EScriptValueType::Vector3;
	Value.Vector = Vector;
	return Value;
}

bool FScriptValue::operator==(const FScriptValue& Other) const
{
	if (Type != Other.Type)
	{
		return false;
	}
	switch (Type)
	{
	case EScriptValueType::Bool:    return bBool == Other.bBool;
	case EScriptValueType::Number:  return Number == Other.Number && bInteger == Other.bInteger;
	case EScriptValueType::String:  return String == Other.String;
	case EScriptValueType::Vector3: return Vector == Other.Vector;
	default:                        return true;
	}
}

FScriptValueMap FScriptProperties::ParseOverrides(std::string_view Json)
{
	FScriptValueMap Result;
	if (Json.empty())
	{
		return Result;
	}

	const nlohmann::json Document = nlohmann::json::parse(Json, nullptr, /*allow_exceptions*/ false);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogScript, Warning, "스크립트 프로퍼티 오버라이드 JSON 형식 오류 — 무시합니다");
		return Result;
	}

	for (const auto& [Name, Value] : Document.items())
	{
		if (Value.is_boolean())
		{
			Result[Name] = FScriptValue::MakeBool(Value.get<bool>());
		}
		else if (Value.is_number_integer())
		{
			Result[Name] = FScriptValue::MakeNumber(static_cast<double>(Value.get<int64_t>()), true);
		}
		else if (Value.is_number())
		{
			Result[Name] = FScriptValue::MakeNumber(Value.get<double>());
		}
		else if (Value.is_string())
		{
			Result[Name] = FScriptValue::MakeString(Value.get<std::string>());
		}
		else if (Value.is_array() && Value.size() == 3 && Value[0].is_number() && Value[1].is_number() && Value[2].is_number())
		{
			Result[Name] = FScriptValue::MakeVector3(FVector3(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>()));
		}
		else
		{
			E_LOG(LogScript, Warning, "지원하지 않는 스크립트 프로퍼티 값 형식 무시: {}", Name);
		}
	}
	return Result;
}

std::string FScriptProperties::SerializeOverrides(const FScriptValueMap& Overrides)
{
	if (Overrides.empty())
	{
		return std::string();
	}

	nlohmann::json Document = nlohmann::json::object();
	for (const auto& [Name, Value] : Overrides)
	{
		switch (Value.Type)
		{
		case EScriptValueType::Bool:    Document[Name] = Value.bBool; break;
		case EScriptValueType::Number:
			if (Value.bInteger)
			{
				Document[Name] = static_cast<int64_t>(Value.Number);
			}
			else
			{
				Document[Name] = Value.Number;
			}
			break;
		case EScriptValueType::String:  Document[Name] = Value.String; break;
		case EScriptValueType::Vector3: Document[Name] = nlohmann::json::array({ Value.Vector.X, Value.Vector.Y, Value.Vector.Z }); break;
		default:                        break;
		}
	}
	return Document.dump();
}

bool FScriptProperties::IsCompatible(const FScriptValue& Default, const FScriptValue& Value)
{
	return Default.Type == Value.Type;
}

#include "Core/Reflection/ReflectionJson.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#pragma warning(push, 0)
#include <json.hpp>
#pragma warning(pop)

namespace
{
	using json = nlohmann::ordered_json;

	bool IsSupported(const FPropertyInfo& Property)
	{
		return Property.Type != EPropertyType::Entity && Property.Type != EPropertyType::ResourceHandle && !Property.HasFlag(PF_Transient);
	}

	template <size_t N>
	json FloatArray(const float (&Values)[N])
	{
		json Array = json::array();
		for (const float Value : Values)
		{
			Array.push_back(Value);
		}
		return Array;
	}

	json ToJson(const FPropertyInfo& Property, const void* Object)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:   return Property.GetRef<bool>(Object);
		case EPropertyType::UInt32: return Property.GetRef<uint32>(Object);
		case EPropertyType::Float:  return Property.GetRef<float>(Object);
		case EPropertyType::String: return Property.GetRef<std::string>(Object);
		case EPropertyType::Int32:
		{
			const int32 Value = Property.GetRef<int32>(Object);
			if (Property.IsEnum() && Value >= 0 && Value < static_cast<int32>(Property.EnumEntries.size()))
			{
				return Property.EnumEntries[static_cast<size_t>(Value)].Name;
			}
			return Value;
		}
		case EPropertyType::Vector2:
		{
			const FVector2& V = Property.GetRef<FVector2>(Object);
			return FloatArray({ V.X, V.Y });
		}
		case EPropertyType::Vector3:
		{
			const FVector3& V = Property.GetRef<FVector3>(Object);
			return FloatArray({ V.X, V.Y, V.Z });
		}
		case EPropertyType::Vector4:
		{
			const FVector4& V = Property.GetRef<FVector4>(Object);
			return FloatArray({ V.X, V.Y, V.Z, V.W });
		}
		case EPropertyType::Quat:
		{
			const FQuat& Q = Property.GetRef<FQuat>(Object);
			return FloatArray({ Q.X, Q.Y, Q.Z, Q.W });
		}
		default:
			return nullptr;
		}
	}

	template <typename T>
	T ClampToRange(const FPropertyInfo& Property, T Value)
	{
		if (Property.HasRange())
		{
			Value = static_cast<T>(std::clamp(static_cast<double>(Value), static_cast<double>(Property.MinValue), static_cast<double>(Property.MaxValue)));
		}
		return Value;
	}

	bool ReadFloats(const json& Value, float* Out, size_t Count)
	{
		if (!Value.is_array() || Value.size() != Count)
		{
			return false;
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (!Value[Index].is_number())
			{
				return false;
			}
			Out[Index] = Value[Index].get<float>();
		}
		return true;
	}

	// 타입이 맞으면 true
	bool FromJson(const FPropertyInfo& Property, void* Object, const json& Value)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:
			if (!Value.is_boolean()) return false;
			Property.GetRef<bool>(Object) = Value.get<bool>();
			return true;
		case EPropertyType::UInt32:
			if (!Value.is_number_integer() || Value.get<int64>() < 0) return false;
			Property.GetRef<uint32>(Object) = ClampToRange(Property, static_cast<uint32>(std::min<int64>(Value.get<int64>(), 0xFFFFFFFFll)));
			return true;
		case EPropertyType::Float:
			if (!Value.is_number() || !std::isfinite(Value.get<double>())) return false;
			Property.GetRef<float>(Object) = ClampToRange(Property, Value.get<float>());
			return true;
		case EPropertyType::String:
			if (!Value.is_string()) return false;
			Property.GetRef<std::string>(Object) = Value.get<std::string>();
			return true;
		case EPropertyType::Int32:
			if (Property.IsEnum() && Value.is_string())
			{
				const int32 EnumValue = Property.FindEnumValue(Value.get<std::string>());
				if (EnumValue < 0) return false;
				Property.GetRef<int32>(Object) = EnumValue;
				return true;
			}
			if (!Value.is_number_integer()) return false;
			if (Property.IsEnum() && (Value.get<int64>() < 0 || Value.get<int64>() >= static_cast<int64>(Property.EnumEntries.size()))) return false;
			Property.GetRef<int32>(Object) = ClampToRange(Property, static_cast<int32>(std::clamp<int64>(Value.get<int64>(), INT32_MIN, INT32_MAX)));
			return true;
		case EPropertyType::Vector2:
		{
			float V[2];
			if (!ReadFloats(Value, V, 2)) return false;
			Property.GetRef<FVector2>(Object) = FVector2(V[0], V[1]);
			return true;
		}
		case EPropertyType::Vector3:
		{
			float V[3];
			if (!ReadFloats(Value, V, 3)) return false;
			Property.GetRef<FVector3>(Object) = FVector3(V[0], V[1], V[2]);
			return true;
		}
		case EPropertyType::Vector4:
		{
			float V[4];
			if (!ReadFloats(Value, V, 4)) return false;
			Property.GetRef<FVector4>(Object) = FVector4(V[0], V[1], V[2], V[3]);
			return true;
		}
		case EPropertyType::Quat:
		{
			float V[4];
			if (!ReadFloats(Value, V, 4)) return false;
			Property.GetRef<FQuat>(Object) = FQuat(V[0], V[1], V[2], V[3]);
			return true;
		}
		default:
			return false;
		}
	}
} // namespace

std::string FReflectionJson::Write(const FTypeInfo& Type, const void* Object)
{
	json Root = json::object();
	for (const FPropertyInfo& Property : Type.Properties)
	{
		if (IsSupported(Property))
		{
			Root[Property.Name] = ToJson(Property, Object);
		}
	}
	return Root.dump(2) + "\n";
}

bool FReflectionJson::Apply(const FTypeInfo& Type, void* Object, std::string_view Json, std::string* OutError)
{
	const json Root = json::parse(Json, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Root.is_discarded() || !Root.is_object())
	{
		if (OutError != nullptr)
		{
			*OutError = "JSON 객체가 아닙니다";
		}
		return false;
	}
	for (const FPropertyInfo& Property : Type.Properties)
	{
		const auto It = Root.find(Property.Name);
		if (It == Root.end() || !IsSupported(Property))
		{
			continue;
		}
		if (!FromJson(Property, Object, *It))
		{
			E_LOG(LogCore, Warning, "{}.{}: 값의 형식이 맞지 않아 무시합니다 ({})", Type.Name, Property.Name, It->dump());
		}
	}
	return true;
}

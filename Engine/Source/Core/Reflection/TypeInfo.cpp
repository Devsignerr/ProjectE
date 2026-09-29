#include "Core/Reflection/TypeInfo.h"

#include "Core/Log.h"

const char* PropertyTypeToString(EPropertyType Type)
{
	switch (Type)
	{
	case EPropertyType::Bool:           return "Bool";
	case EPropertyType::Int32:          return "Int32";
	case EPropertyType::UInt32:         return "UInt32";
	case EPropertyType::Float:          return "Float";
	case EPropertyType::String:         return "String";
	case EPropertyType::Vector2:        return "Vector2";
	case EPropertyType::Vector3:        return "Vector3";
	case EPropertyType::Vector4:        return "Vector4";
	case EPropertyType::Quat:           return "Quat";
	case EPropertyType::Entity:         return "Entity";
	case EPropertyType::ResourceHandle: return "ResourceHandle";
	default:                            return "Unknown";
	}
}

FTypeRegistry& FTypeRegistry::Get()
{
	static FTypeRegistry Instance;
	return Instance;
}

const FTypeInfo* FTypeRegistry::Find(std::string_view Name) const
{
	const auto Found = ByName.find(std::string(Name));
	return Found != ByName.end() ? Found->second : nullptr;
}

FTypeInfo& FTypeRegistry::AddType(std::type_index Index, std::string Name, std::string DisplayName, size_t Size, size_t Alignment)
{
	E_CHECKF(!ByName.contains(Name), "타입 이름 '{}'가 이미 사용 중입니다", Name);

	auto Info         = std::make_unique<FTypeInfo>();
	Info->Name        = std::move(Name);
	Info->DisplayName = std::move(DisplayName);
	Info->TypeId      = static_cast<uint32>(Types.size());
	Info->Size        = Size;
	Info->Alignment   = Alignment;
	Info->Owner       = CurrentOwner;

	FTypeInfo& Result = *Info;
	ByName[Result.Name] = &Result;
	ByType[Index]       = &Result;
	Types.push_back(std::move(Info));

	E_LOG(LogCore, Verbose, "리플렉션 타입 등록: {} ({} bytes)", Result.Name, Result.Size);
	return Result;
}

size_t FTypeRegistry::RemoveTypesByOwner(std::string_view Owner)
{
	size_t Removed = 0;
	for (auto It = Types.begin(); It != Types.end();)
	{
		FTypeInfo& Type = **It;
		if (Type.Owner != Owner)
		{
			++It;
			continue;
		}
		ByName.erase(Type.Name);
		std::erase_if(ByType, [&Type](const auto& Entry) { return Entry.second == &Type; });
		It = Types.erase(It);
		++Removed;
	}
	for (size_t Index = 0; Index < Types.size(); ++Index)
	{
		Types[Index]->TypeId = static_cast<uint32>(Index);
	}
	return Removed;
}

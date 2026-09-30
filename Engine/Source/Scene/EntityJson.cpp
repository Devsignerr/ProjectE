#include "Scene/EntityJson.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/Scene.h"

#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;

	// ---- 프로퍼티 → JSON

	json PropertyToJson(const FPropertyInfo& Property, const void* Object, const std::unordered_map<uint64, int32>& EntityIndices)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:    return Property.GetRef<bool>(Object);
		case EPropertyType::Int32:   return Property.GetRef<int32>(Object);
		case EPropertyType::UInt32:  return Property.GetRef<uint32>(Object);
		case EPropertyType::Float:   return Property.GetRef<float>(Object);
		case EPropertyType::String:  return Property.GetRef<std::string>(Object);
		case EPropertyType::Vector2:
		{
			const FVector2& V = Property.GetRef<FVector2>(Object);
			return json::array({ V.X, V.Y });
		}
		case EPropertyType::Vector3:
		{
			const FVector3& V = Property.GetRef<FVector3>(Object);
			return json::array({ V.X, V.Y, V.Z });
		}
		case EPropertyType::Vector4:
		{
			const FVector4& V = Property.GetRef<FVector4>(Object);
			return json::array({ V.X, V.Y, V.Z, V.W });
		}
		case EPropertyType::Quat:
		{
			const FQuat& Q = Property.GetRef<FQuat>(Object);
			return json::array({ Q.X, Q.Y, Q.Z, Q.W });
		}
		case EPropertyType::Entity:
		{
			const FEntity& Entity = Property.GetRef<FEntity>(Object);
			const auto     Found  = EntityIndices.find(Entity.ToId());
			return Found != EntityIndices.end() ? Found->second : -1;
		}
		default:
			return nullptr;
		}
	}

	// ---- JSON → 프로퍼티

	bool ReadFloatArray(const json& Value, float* Out, size_t Count)
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

	bool JsonToProperty(const FPropertyInfo& Property, void* Object, const json& Value, const std::vector<FEntity>& Entities)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:
			if (!Value.is_boolean()) return false;
			Property.GetRef<bool>(Object) = Value.get<bool>();
			return true;
		case EPropertyType::Int32:
			if (!Value.is_number()) return false;
			Property.GetRef<int32>(Object) = Value.get<int32>();
			return true;
		case EPropertyType::UInt32:
			if (!Value.is_number()) return false;
			Property.GetRef<uint32>(Object) = Value.get<uint32>();
			return true;
		case EPropertyType::Float:
			if (!Value.is_number()) return false;
			Property.GetRef<float>(Object) = Value.get<float>();
			return true;
		case EPropertyType::String:
			if (!Value.is_string()) return false;
			Property.GetRef<std::string>(Object) = Value.get<std::string>();
			return true;
		case EPropertyType::Vector2: return ReadFloatArray(Value, &Property.GetRef<FVector2>(Object).X, 2);
		case EPropertyType::Vector3: return ReadFloatArray(Value, &Property.GetRef<FVector3>(Object).X, 3);
		case EPropertyType::Vector4: return ReadFloatArray(Value, &Property.GetRef<FVector4>(Object).X, 4);
		case EPropertyType::Quat:
		{
			FQuat& Q = Property.GetRef<FQuat>(Object);
			if (!ReadFloatArray(Value, &Q.X, 4)) return false;
			// 손으로 편집된 값만 보정 (정상 값을 다시 정규화하면 최하위 비트가 흔들려 재저장 결과가 달라진다)
			if (!Q.IsNormalized(1.0e-3f))
			{
				Q.Normalize();
			}
			return true;
		}
		case EPropertyType::Entity:
		{
			if (!Value.is_number_integer()) return false;
			const int32 Index = Value.get<int32>();
			Property.GetRef<FEntity>(Object) = (Index >= 0 && Index < static_cast<int32>(Entities.size())) ? Entities[Index] : NullEntity;
			return true;
		}
		default:
			return false;
		}
	}
} // namespace

bool FEntityJson::IsStructuralType(const FTypeInfo& Type)
{
	const FTypeRegistry& Registry = FTypeRegistry::Get();
	return &Type == Registry.Find<FNameComponent>() || &Type == Registry.Find<FHierarchyComponent>() ||
	       &Type == Registry.Find<FTransientComponent>();
}

bool FEntityJson::IsSerializable(const FPropertyInfo& Property)
{
	return !Property.HasFlag(PF_Transient) && Property.Type != EPropertyType::ResourceHandle;
}

void FEntityJson::CollectSubtree(FScene& Scene, FEntity Root, std::vector<FEntity>& Out)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Root) || Registry.Has<FTransientComponent>(Root))
	{
		return;
	}
	Out.push_back(Root);
	for (FEntity Child : Scene.GetChildren(Root))
	{
		CollectSubtree(Scene, Child, Out);
	}
}

json FEntityJson::Write(FScene& Scene, const std::vector<FEntity>& Entities)
{
	FRegistry& Registry = Scene.GetRegistry();

	std::unordered_map<uint64, int32> EntityIndices;
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		EntityIndices[Entities[Index].ToId()] = static_cast<int32>(Index);
	}

	json Array = json::array();
	for (FEntity Entity : Entities)
	{
		json EntityJson;
		const FNameComponent* Name = Registry.TryGet<FNameComponent>(Entity);
		EntityJson["Name"] = Name ? Name->Name : std::string();

		const FEntity Parent = Scene.GetParent(Entity);
		const auto    Found  = EntityIndices.find(Parent.ToId());
		EntityJson["Parent"] = (Parent.IsValid() && Found != EntityIndices.end()) ? Found->second : -1;

		json Components = json::object();
		FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
			if (IsStructuralType(Type) || !Type.HasComponent(Registry, Entity))
			{
				return;
			}
			const void* Component  = Type.GetComponent(Registry, Entity);
			json        Properties = json::object();
			for (const FPropertyInfo& Property : Type.Properties)
			{
				if (IsSerializable(Property))
				{
					Properties[Property.Name] = PropertyToJson(Property, Component, EntityIndices);
				}
			}
			Components[Type.Name] = std::move(Properties);
		});
		EntityJson["Components"] = std::move(Components);

		Array.push_back(std::move(EntityJson));
	}
	return Array;
}

std::vector<FEntity> FEntityJson::Read(FScene& Scene, const json& Array, FEntity RootParent, std::string_view SourceLabel)
{
	FRegistry&           Registry = Scene.GetRegistry();
	const FTypeRegistry& Types    = FTypeRegistry::Get();

	// 1단계: 엔티티 생성 (이름) — 인덱스 참조 해석을 위해 먼저 전부 만든다
	std::vector<FEntity> Entities;
	Entities.reserve(Array.size());
	for (const json& EntityJson : Array)
	{
		Entities.push_back(Scene.CreateEntity(EntityJson.is_object() ? EntityJson.value("Name", "Entity") : std::string("Entity")));
	}

	// 2단계: 계층 + 컴포넌트
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const json&   EntityJson = Array[Index];
		const FEntity Entity     = Entities[Index];
		if (!EntityJson.is_object())
		{
			continue;
		}

		const int32 ParentIndex = EntityJson.value("Parent", -1);
		if (ParentIndex >= 0 && ParentIndex < static_cast<int32>(Entities.size()) && ParentIndex != static_cast<int32>(Index))
		{
			Scene.SetParent(Entity, Entities[ParentIndex]);
		}
		else if (RootParent.IsValid())
		{
			Scene.SetParent(Entity, RootParent);
		}

		const auto ComponentsIt = EntityJson.find("Components");
		if (ComponentsIt == EntityJson.end() || !ComponentsIt->is_object())
		{
			continue;
		}
		for (const auto& [TypeName, Properties] : ComponentsIt->items())
		{
			const FTypeInfo* Type = Types.Find(TypeName);
			if (Type == nullptr || !Type->bIsComponent)
			{
				E_LOG(LogScene, Warning, "{}: 알 수 없는 컴포넌트 '{}' 무시 (엔티티 '{}')", SourceLabel, TypeName, EntityJson.value("Name", ""));
				continue;
			}
			void* Component = Type->AddComponent(Registry, Entity);
			if (!Properties.is_object())
			{
				continue;
			}
			for (const auto& [PropertyName, Value] : Properties.items())
			{
				const FPropertyInfo* Property = Type->FindProperty(PropertyName);
				if (Property == nullptr || !IsSerializable(*Property))
				{
					E_LOG(LogScene, Warning, "{}: 알 수 없는 프로퍼티 '{}.{}' 무시", SourceLabel, TypeName, PropertyName);
					continue;
				}
				if (!JsonToProperty(*Property, Component, Value, Entities))
				{
					E_LOG(LogScene, Warning, "{}: 프로퍼티 '{}.{}' 값 형식이 맞지 않아 무시", SourceLabel, TypeName, PropertyName);
				}
			}
		}
	}
	return Entities;
}

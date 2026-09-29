#include "Scene/SceneSerializer.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Scene/Scene.h"

#include <json.hpp>

#include <fstream>
#include <sstream>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;

	bool IsStructuralType(const FTypeInfo& Type)
	{
		const FTypeRegistry& Registry = FTypeRegistry::Get();
		return &Type == Registry.Find<FNameComponent>() || &Type == Registry.Find<FHierarchyComponent>() ||
		       &Type == Registry.Find<FTransientComponent>();
	}

	bool IsSerializable(const FPropertyInfo& Property)
	{
		return !Property.HasFlag(PF_Transient) && Property.Type != EPropertyType::ResourceHandle;
	}

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

	// 부모 → 자식 순서로 직렬화 대상 엔티티 수집 (Transient 하위 트리 제외)
	void CollectEntities(FScene& Scene, FEntity Entity, std::vector<FEntity>& Out)
	{
		FRegistry& Registry = Scene.GetRegistry();
		if (Registry.Has<FTransientComponent>(Entity))
		{
			return;
		}
		Out.push_back(Entity);
		for (FEntity Child : Scene.GetChildren(Entity))
		{
			CollectEntities(Scene, Child, Out);
		}
	}
} // namespace

std::string FSceneSerializer::ToJsonString(FScene& Scene)
{
	FRegistry& Registry = Scene.GetRegistry();

	std::vector<FEntity> Entities;
	for (FEntity Root : Scene.GetRootEntities())
	{
		CollectEntities(Scene, Root, Entities);
	}

	std::unordered_map<uint64, int32> EntityIndices;
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		EntityIndices[Entities[Index].ToId()] = static_cast<int32>(Index);
	}

	json Document;
	Document["Version"]  = Version;
	Document["Entities"] = json::array();

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

		Document["Entities"].push_back(std::move(EntityJson));
	}

	return Document.dump(2);
}

bool FSceneSerializer::FromJsonString(FScene& OutScene, const std::string& JsonText)
{
	const json Document = json::parse(JsonText, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogScene, Error, "씬 JSON 파싱 실패");
		return false;
	}

	const int32 FileVersion = Document.value("Version", 0);
	if (FileVersion > Version)
	{
		E_LOG(LogScene, Warning, "씬 파일 버전 {}이(가) 지원 버전 {}보다 높습니다. 일부 데이터가 무시될 수 있습니다", FileVersion, Version);
	}

	const auto EntitiesIt = Document.find("Entities");
	if (EntitiesIt == Document.end() || !EntitiesIt->is_array())
	{
		E_LOG(LogScene, Error, "씬 JSON에 Entities 배열이 없습니다");
		return false;
	}

	OutScene.Clear();
	FRegistry&           Registry = OutScene.GetRegistry();
	const FTypeRegistry& Types    = FTypeRegistry::Get();

	// 1단계: 엔티티 생성 (이름) — 인덱스 참조 해석을 위해 먼저 전부 만든다
	std::vector<FEntity> Entities;
	Entities.reserve(EntitiesIt->size());
	for (const json& EntityJson : *EntitiesIt)
	{
		Entities.push_back(OutScene.CreateEntity(EntityJson.value("Name", "Entity")));
	}

	// 2단계: 계층 + 컴포넌트
	for (size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const json&   EntityJson = (*EntitiesIt)[Index];
		const FEntity Entity     = Entities[Index];

		const int32 ParentIndex = EntityJson.value("Parent", -1);
		if (ParentIndex >= 0 && ParentIndex < static_cast<int32>(Entities.size()) && ParentIndex != static_cast<int32>(Index))
		{
			OutScene.SetParent(Entity, Entities[ParentIndex]);
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
				E_LOG(LogScene, Warning, "알 수 없는 컴포넌트 '{}' 무시 (엔티티 '{}')", TypeName, EntityJson.value("Name", ""));
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
					E_LOG(LogScene, Warning, "알 수 없는 프로퍼티 '{}.{}' 무시", TypeName, PropertyName);
					continue;
				}
				if (!JsonToProperty(*Property, Component, Value, Entities))
				{
					E_LOG(LogScene, Warning, "프로퍼티 '{}.{}' 값 형식이 맞지 않아 무시", TypeName, PropertyName);
				}
			}
		}
	}

	OutScene.UpdateTransforms();
	E_LOG(LogScene, Display, "씬 로드: 엔티티 {}개", Entities.size());
	return true;
}

bool FSceneSerializer::SaveToFile(FScene& Scene, const std::filesystem::path& Path)
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);

	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogScene, Error, "씬 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString(Scene);
	E_LOG(LogScene, Display, "씬 저장: {}", FStringConv::ToUtf8(Path.wstring()));
	return true;
}

bool FSceneSerializer::LoadFromFile(FScene& OutScene, const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		E_LOG(LogScene, Error, "씬 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	if (!FromJsonString(OutScene, Buffer.str()))
	{
		E_LOG(LogScene, Error, "씬 로드 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	return true;
}

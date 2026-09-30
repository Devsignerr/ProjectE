#include "Editor/SceneEditOps.h"

#include "Core/Math/Box.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"
#include "Scene/SceneSerializer.h"

#include <charconv>
#include <cstring>
#include <unordered_map>

namespace
{
	void CopyProperty(const FPropertyInfo& Property, const void* Source, void* Dest)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:    Property.GetRef<bool>(Dest)        = Property.GetRef<bool>(Source); break;
		case EPropertyType::Int32:   Property.GetRef<int32>(Dest)       = Property.GetRef<int32>(Source); break;
		case EPropertyType::UInt32:  Property.GetRef<uint32>(Dest)      = Property.GetRef<uint32>(Source); break;
		case EPropertyType::Float:   Property.GetRef<float>(Dest)       = Property.GetRef<float>(Source); break;
		case EPropertyType::String:  Property.GetRef<std::string>(Dest) = Property.GetRef<std::string>(Source); break;
		case EPropertyType::Vector2: Property.GetRef<FVector2>(Dest)    = Property.GetRef<FVector2>(Source); break;
		case EPropertyType::Vector3: Property.GetRef<FVector3>(Dest)    = Property.GetRef<FVector3>(Source); break;
		case EPropertyType::Vector4: Property.GetRef<FVector4>(Dest)    = Property.GetRef<FVector4>(Source); break;
		case EPropertyType::Quat:    Property.GetRef<FQuat>(Dest)       = Property.GetRef<FQuat>(Source); break;
		case EPropertyType::Entity:  Property.GetRef<FEntity>(Dest)     = Property.GetRef<FEntity>(Source); break;
		case EPropertyType::ResourceHandle:
			// 핸들은 트리비얼한 (Index, Generation) 값
			std::memcpy(Property.GetPtr(Dest), Property.GetPtr(Source), Property.Size);
			break;
		default:
			break;
		}
	}

	FEntity CloneRecursive(FScene& SourceScene, FEntity Source, FScene& DestScene, FEntity DestParent,
	                       std::unordered_map<uint64, FEntity>& OutMap)
	{
		FRegistry& SourceRegistry = SourceScene.GetRegistry();
		if (!SourceRegistry.IsValid(Source))
		{
			return NullEntity;
		}

		// 같은 씬 복제 시 엔티티/컴포넌트 추가가 저장소를 재할당할 수 있으므로 원본 포인터는 추가 이후에 얻는다
		FRegistry&            DestRegistry = DestScene.GetRegistry();
		const FNameComponent* SourceName   = SourceRegistry.TryGet<FNameComponent>(Source);
		const std::string     Name         = SourceName ? SourceName->Name : std::string("Entity");
		const FEntity         Clone        = DestScene.CreateEntity(Name);
		DestScene.SetParent(Clone, DestParent);
		OutMap[Source.ToId()] = Clone;

		const FTypeInfo* HierarchyType = FTypeRegistry::Get().Find<FHierarchyComponent>();
		FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
			// 계층은 SetParent로 새로 구성한다 (원본 엔티티 참조를 복사하면 안 됨)
			if (&Type == HierarchyType || !Type.HasComponent(SourceRegistry, Source))
			{
				return;
			}
			void*       DestComponent   = Type.HasComponent(DestRegistry, Clone) ? Type.GetComponent(DestRegistry, Clone) : Type.AddComponent(DestRegistry, Clone);
			const void* SourceComponent = Type.GetComponent(SourceRegistry, Source);
			for (const FPropertyInfo& Property : Type.Properties)
			{
				CopyProperty(Property, SourceComponent, DestComponent);
			}
		});

		const std::vector<FEntity> Children = SourceScene.GetChildren(Source); // 같은 씬 복제 시 원본 목록이 바뀌지 않도록 복사
		for (FEntity Child : Children)
		{
			CloneRecursive(SourceScene, Child, DestScene, Clone, OutMap);
		}
		return Clone;
	}

	// 리플렉션 Entity 프로퍼티 중 복제 범위 안을 가리키는 참조를 복제본으로 바꾼다.
	// 프리팹 연결: 인스턴스 루트까지 복제하면 새 인스턴스, 인스턴스 일부만 복제하면 연결을 끊어 인스턴스에 추가한 엔티티가 된다
	void RemapReflectedReferences(FScene& DestScene, const std::unordered_map<uint64, FEntity>& Map)
	{
		FRegistry& Registry = DestScene.GetRegistry();
		for (const auto& [SourceId, CloneEntity] : Map)
		{
			(void)SourceId;
			// 연결 판정은 다시 매핑하기 전 (원본 인스턴스 루트가 복제 범위에 있었는가)
			const FPrefabLinkComponent* Link         = Registry.TryGet<FPrefabLinkComponent>(CloneEntity);
			const bool                  bBrokenLink  = Link != nullptr && !Map.contains(Link->Root.ToId());
			FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
				if (!Type.HasComponent(Registry, CloneEntity))
				{
					return;
				}
				for (const FPropertyInfo& Property : Type.Properties)
				{
					if (Property.Type == EPropertyType::Entity)
					{
						FEntity&   Reference = Property.GetRef<FEntity>(Type.GetComponent(Registry, CloneEntity));
						const auto Found     = Map.find(Reference.ToId());
						if (Found != Map.end())
						{
							Reference = Found->second;
						}
					}
				}
			});
			if (bBrokenLink)
			{
				Registry.Remove<FPrefabLinkComponent>(CloneEntity);
				if (Registry.Has<FPrefabInstanceComponent>(CloneEntity))
				{
					Registry.Remove<FPrefabInstanceComponent>(CloneEntity); // 중첩 인스턴스 일부 복제 → 일반 엔티티
				}
			}
		}
	}

	// 리플렉션 밖 런타임 데이터(스킨 관절/애니메이션 노드)를 복사하고 Map 안의 참조는 복제본으로 바꾼다
	void CopyAndRemapRuntimeData(FScene& SourceScene, FScene& DestScene, const std::unordered_map<uint64, FEntity>& Map)
	{
		const auto Remap = [&Map](FEntity Entity) {
			const auto Found = Map.find(Entity.ToId());
			return Found != Map.end() ? Found->second : Entity;
		};
		for (const auto& [SourceId, CloneEntity] : Map)
		{
			FSceneCloner::CopyRuntimeData(SourceScene.GetRegistry(), FEntity::FromId(SourceId), DestScene.GetRegistry(), CloneEntity);
			FSceneCloner::RemapRuntimeReferences(DestScene.GetRegistry(), CloneEntity, Remap);
		}
		RemapReflectedReferences(DestScene, Map);
	}
} // namespace

FEntity FSceneEditOps::CloneSubtree(FScene& SourceScene, FEntity Source, FScene& DestScene, FEntity DestParent)
{
	std::unordered_map<uint64, FEntity> Map;
	const FEntity                       Clone = CloneRecursive(SourceScene, Source, DestScene, DestParent, Map);
	CopyAndRemapRuntimeData(SourceScene, DestScene, Map);
	return Clone;
}

void FSceneEditOps::CloneChildren(FScene& SourceScene, FEntity SourceParent, FScene& DestScene, FEntity DestParent)
{
	// 자식 서브트리를 한 맵으로 복제해야 형제 사이 참조(메시 → 뼈대 관절)가 복제본으로 이어진다
	std::unordered_map<uint64, FEntity> Map;
	Map[SourceParent.ToId()] = DestParent;
	const std::vector<FEntity> Children = SourceScene.GetChildren(SourceParent);
	for (FEntity Child : Children)
	{
		CloneRecursive(SourceScene, Child, DestScene, DestParent, Map);
	}
	CopyAndRemapRuntimeData(SourceScene, DestScene, Map);
}

std::vector<FEntity> FSceneEditOps::GetTopLevel(const FScene& Scene, const std::vector<FEntity>& Entities)
{
	std::vector<FEntity> Result;
	Result.reserve(Entities.size());
	for (FEntity Entity : Entities)
	{
		if (!Scene.GetRegistry().IsValid(Entity))
		{
			continue;
		}
		bool bHasSelectedAncestor = false;
		for (FEntity Other : Entities)
		{
			if (Other != Entity && Scene.IsAncestorOf(Other, Entity))
			{
				bHasSelectedAncestor = true;
				break;
			}
		}
		if (!bHasSelectedAncestor)
		{
			Result.push_back(Entity);
		}
	}
	return Result;
}

namespace
{
	// 복제/붙여넣기 결과 루트 이름에 번호를 붙여 씬에서 겹치지 않게 한다
	void MakeRootNameUnique(FScene& Scene, FEntity Root)
	{
		FNameComponent* Name = Scene.GetRegistry().TryGet<FNameComponent>(Root);
		if (Name == nullptr)
		{
			return;
		}
		const auto IsUsed = [&Scene, Root](const std::string& Candidate) {
			bool bUsed = false;
			Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Other) {
				bUsed = bUsed || (Entity != Root && Other.Name == Candidate);
			});
			return bUsed;
		};
		if (IsUsed(Name->Name))
		{
			Name->Name = FSceneEditOps::MakeUniqueName(Name->Name, IsUsed);
		}
	}
} // namespace

std::vector<FEntity> FSceneEditOps::Duplicate(FScene& Scene, const std::vector<FEntity>& Entities)
{
	std::vector<FEntity> Clones;
	for (FEntity Entity : GetTopLevel(Scene, Entities))
	{
		const FEntity Clone = CloneSubtree(Scene, Entity, Scene, Scene.GetParent(Entity));
		MakeRootNameUnique(Scene, Clone);
		Clones.push_back(Clone);
	}
	Scene.UpdateTransforms();
	return Clones;
}

std::string FSceneEditOps::Copy(FScene& Scene, const std::vector<FEntity>& Entities)
{
	const std::vector<FEntity> TopLevel = GetTopLevel(Scene, Entities);
	if (TopLevel.empty())
	{
		return {};
	}
	// 임시 씬에 루트로 복제하고 월드 트랜스폼을 로컬 값으로 굳힌다 (붙여넣으면 원래 보이던 자리에 놓인다)
	FScene Clipboard;
	for (FEntity Entity : TopLevel)
	{
		const FEntity Clone = CloneSubtree(Scene, Entity, Clipboard, NullEntity);
		if (Clone.IsValid())
		{
			FTransformComponent& Transform = Clipboard.GetTransform(Clone);
			Scene.GetTransform(Entity).WorldMatrix.Decompose(Transform.Position, Transform.Rotation, Transform.Scale);
		}
	}
	return FSceneSerializer::ToJsonString(Clipboard);
}

std::vector<FEntity> FSceneEditOps::Paste(FScene& Scene, const std::string& Clipboard)
{
	std::vector<FEntity> Roots;
	FScene               Source;
	if (Clipboard.empty() || !FSceneSerializer::FromJsonString(Source, Clipboard))
	{
		return Roots;
	}
	for (FEntity Entity : Source.GetRootEntities())
	{
		const FEntity Clone = CloneSubtree(Source, Entity, Scene, NullEntity);
		MakeRootNameUnique(Scene, Clone);
		Roots.push_back(Clone);
	}
	Scene.UpdateTransforms();
	return Roots;
}

bool FSceneEditOps::FindFloorHeight(const FBox& Bounds, const std::vector<FBox>& Surfaces, float& OutHeight)
{
	const float Limit = Bounds.GetCenter().Z;
	bool        bFound = false;
	for (const FBox& Surface : Surfaces)
	{
		const bool bOverlapsXY = Surface.Min.X <= Bounds.Max.X && Surface.Max.X >= Bounds.Min.X && Surface.Min.Y <= Bounds.Max.Y &&
		                         Surface.Max.Y >= Bounds.Min.Y;
		if (!Surface.IsValid() || !bOverlapsXY || Surface.Max.Z > Limit)
		{
			continue;
		}
		if (!bFound || Surface.Max.Z > OutHeight)
		{
			OutHeight = Surface.Max.Z;
			bFound    = true;
		}
	}
	return bFound;
}

void FSceneEditOps::Delete(FScene& Scene, const std::vector<FEntity>& Entities)
{
	for (FEntity Entity : GetTopLevel(Scene, Entities))
	{
		Scene.DestroyEntity(Entity);
	}
}

std::string FSceneEditOps::MakeUniqueName(std::string_view Name, const std::function<bool(const std::string&)>& IsUsed)
{
	// 끝의 숫자 분리 ("Rock_07" → "Rock_", 7, 자릿수 2)
	size_t DigitStart = Name.size();
	while (DigitStart > 0 && Name[DigitStart - 1] >= '0' && Name[DigitStart - 1] <= '9')
	{
		--DigitStart;
	}
	const std::string_view Base   = Name.substr(0, DigitStart);
	const std::string_view Digits = Name.substr(DigitStart);

	uint32 Number = 0;
	if (!Digits.empty())
	{
		std::from_chars(Digits.data(), Digits.data() + Digits.size(), Number);
	}
	const size_t Width = Digits.size();

	for (uint32 Candidate = Number + 1;; ++Candidate)
	{
		std::string Suffix = std::to_string(Candidate);
		if (Suffix.size() < Width)
		{
			Suffix.insert(0, Width - Suffix.size(), '0');
		}
		std::string Result = std::string(Base) + Suffix;
		if (!IsUsed(Result))
		{
			return Result;
		}
	}
}

// ---------------------------------------------------------------- FEntityPath

namespace
{
	const std::string& GetEntityName(const FScene& Scene, FEntity Entity)
	{
		static const std::string Empty;
		const FNameComponent* Name = Scene.GetRegistry().TryGet<FNameComponent>(Entity);
		return Name ? Name->Name : Empty;
	}

	// Siblings 중 Entity 앞에 같은 이름이 몇 개 있는지
	uint32 CountOccurrence(const FScene& Scene, const std::vector<FEntity>& Siblings, FEntity Entity)
	{
		const std::string& Name  = GetEntityName(Scene, Entity);
		uint32             Count = 0;
		for (FEntity Sibling : Siblings)
		{
			if (Sibling == Entity)
			{
				break;
			}
			Count += GetEntityName(Scene, Sibling) == Name ? 1u : 0u;
		}
		return Count;
	}
} // namespace

FEntityPath FEntityPath::Build(const FScene& Scene, FEntity Entity)
{
	FEntityPath Path;
	if (!Scene.GetRegistry().IsValid(Entity))
	{
		return Path;
	}
	for (FEntity Current = Entity; Current.IsValid(); Current = Scene.GetParent(Current))
	{
		const FEntity              Parent   = Scene.GetParent(Current);
		const std::vector<FEntity> Siblings = Parent.IsValid() ? Scene.GetChildren(Parent) : Scene.GetRootEntities();
		Path.Segments.insert(Path.Segments.begin(), FSegment{ GetEntityName(Scene, Current), CountOccurrence(Scene, Siblings, Current) });
	}
	return Path;
}

FEntity FEntityPath::Resolve(const FScene& Scene) const
{
	FEntity Current;
	for (const FSegment& Segment : Segments)
	{
		const std::vector<FEntity> Candidates = Current.IsValid() ? Scene.GetChildren(Current) : Scene.GetRootEntities();
		FEntity                    Found;
		uint32                     Seen = 0;
		for (FEntity Candidate : Candidates)
		{
			if (GetEntityName(Scene, Candidate) == Segment.Name && Seen++ == Segment.Occurrence)
			{
				Found = Candidate;
				break;
			}
		}
		if (!Found.IsValid())
		{
			return NullEntity;
		}
		Current = Found;
	}
	return Current;
}

#include "Scene/SceneCloner.h"

#include "Core/Reflection/TypeInfo.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

void FSceneCloner::Clone(const FScene& Source, FScene& Dest, FEntityMap* OutEntityMap)
{
	Dest.Clear();

	const FRegistry& SourceRegistry = Source.GetRegistry();
	FRegistry&       DestRegistry   = Dest.GetRegistry();

	const TSparseSet<FHierarchyComponent>* HierarchyPool = SourceRegistry.TryGetPool<FHierarchyComponent>();
	if (HierarchyPool == nullptr)
	{
		return;
	}
	const std::vector<FEntity> SourceEntities = HierarchyPool->GetEntities(); // 풀 순서 = 루트/자식 표시 순서

	FEntityMap EntityMap;
	EntityMap.reserve(SourceEntities.size());
	for (FEntity SourceEntity : SourceEntities)
	{
		EntityMap[SourceEntity.ToId()] = DestRegistry.Create();
	}

	const auto Remap = [&](FEntity Entity) {
		const auto Found = EntityMap.find(Entity.ToId());
		return Found != EntityMap.end() ? Found->second : NullEntity;
	};

	// 컴포넌트 값 복사 (등록 순서대로 → 풀 순서가 원본과 같아진다)
	const FTypeRegistry& Types = FTypeRegistry::Get();
	Types.ForEachComponentType([&](const FTypeInfo& Type) {
		if (!Type.CopyComponent)
		{
			return;
		}
		for (FEntity SourceEntity : SourceEntities)
		{
			if (Type.HasComponent(SourceRegistry, SourceEntity))
			{
				Type.CopyComponent(DestRegistry, EntityMap[SourceEntity.ToId()], SourceRegistry, SourceEntity);
			}
		}
	});

	// 엔티티 참조 다시 매핑 (+ 리플렉션 밖 런타임 데이터)
	for (FEntity SourceEntity : SourceEntities)
	{
		const FEntity DestEntity = EntityMap[SourceEntity.ToId()];
		CopyRuntimeData(SourceRegistry, SourceEntity, DestRegistry, DestEntity);
		RemapRuntimeReferences(DestRegistry, DestEntity, Remap);
		if (FHierarchyComponent* Hierarchy = DestRegistry.TryGet<FHierarchyComponent>(DestEntity))
		{
			Hierarchy->Parent = Remap(Hierarchy->Parent);
			for (FEntity& Child : Hierarchy->Children)
			{
				Child = Remap(Child);
			}
			std::erase_if(Hierarchy->Children, [](FEntity Child) { return !Child.IsValid(); });
		}

		Types.ForEachComponentType([&](const FTypeInfo& Type) {
			if (!Type.HasComponent(DestRegistry, DestEntity))
			{
				return;
			}
			for (const FPropertyInfo& Property : Type.Properties)
			{
				if (Property.Type == EPropertyType::Entity)
				{
					FEntity& Reference = Property.GetRef<FEntity>(Type.GetComponent(DestRegistry, DestEntity));
					Reference          = Remap(Reference);
				}
			}
		});
	}

	if (OutEntityMap != nullptr)
	{
		*OutEntityMap = std::move(EntityMap);
	}
}

void FSceneCloner::CopyRuntimeData(const FRegistry& Source, FEntity SourceEntity, FRegistry& Dest, FEntity DestEntity)
{
	// 같은 레지스트리면 Emplace가 풀을 재할당할 수 있으므로 값을 먼저 복사해 둔다
	if (const FSkinComponent* Skin = Source.TryGet<FSkinComponent>(SourceEntity))
	{
		FSkinComponent Copy = *Skin;
		Dest.GetOrEmplace<FSkinComponent>(DestEntity) = std::move(Copy);
	}
	if (const FAnimationComponent* Animation = Source.TryGet<FAnimationComponent>(SourceEntity))
	{
		if (FAnimationComponent* DestAnimation = Dest.TryGet<FAnimationComponent>(DestEntity); DestAnimation && DestAnimation != Animation)
		{
			DestAnimation->Runtime = Animation->Runtime;
		}
	}
}

void FSceneCloner::RemapRuntimeReferences(FRegistry& Registry, FEntity Entity, const std::function<FEntity(FEntity)>& Remap)
{
	if (FSkinComponent* Skin = Registry.TryGet<FSkinComponent>(Entity))
	{
		for (FEntity& Joint : Skin->Joints)
		{
			Joint = Remap(Joint);
		}
	}
	if (FAnimationComponent* Animation = Registry.TryGet<FAnimationComponent>(Entity))
	{
		for (FEntity& Node : Animation->Runtime.NodeEntities)
		{
			Node = Remap(Node);
		}
	}
}

#include "Scene/Scene.h"

#include "Core/Log.h"
#include "Scene/SceneReflection.h"

#include <algorithm>

E_DECLARE_LOG_CATEGORY(LogScene)
E_DEFINE_LOG_CATEGORY(LogScene, Log)

namespace
{
	const std::vector<FEntity> GEmptyChildren;
}

FScene::FScene()
{
	RegisterSceneTypes();
}

FEntity FScene::CreateEntity(std::string_view Name)
{
	const FEntity Entity = Registry.Create();
	Registry.Emplace<FNameComponent>(Entity, std::string(Name));
	Registry.Emplace<FTransformComponent>(Entity);
	Registry.Emplace<FHierarchyComponent>(Entity);
	return Entity;
}

void FScene::DestroyEntity(FEntity Entity)
{
	if (!Registry.IsValid(Entity))
	{
		return;
	}

	// 자식 먼저 (목록을 복사해 순회 중 변경 회피)
	if (FHierarchyComponent* Hierarchy = Registry.TryGet<FHierarchyComponent>(Entity))
	{
		const std::vector<FEntity> Children = Hierarchy->Children;
		for (FEntity Child : Children)
		{
			DestroyEntity(Child);
		}
	}

	SetParent(Entity, NullEntity);
	Registry.Destroy(Entity);
}

void FScene::SetParent(FEntity Child, FEntity Parent)
{
	FHierarchyComponent* ChildHierarchy = Registry.TryGet<FHierarchyComponent>(Child);
	if (ChildHierarchy == nullptr)
	{
		return;
	}
	if (ChildHierarchy->Parent == Parent)
	{
		return;
	}
	if (Parent.IsValid() && (Parent == Child || IsAncestorOf(Child, Parent)))
	{
		E_LOG(LogScene, Warning, "순환 계층은 허용되지 않습니다 (엔티티 {} → {})", Child.Index, Parent.Index);
		return;
	}

	// 기존 부모에서 제거
	if (FHierarchyComponent* OldParent = Registry.TryGet<FHierarchyComponent>(ChildHierarchy->Parent))
	{
		std::erase(OldParent->Children, Child);
	}

	// 새 부모에 추가
	if (Parent.IsValid())
	{
		FHierarchyComponent* NewParent = Registry.TryGet<FHierarchyComponent>(Parent);
		if (NewParent == nullptr)
		{
			E_LOG(LogScene, Warning, "부모 엔티티 {}에 계층 컴포넌트가 없습니다", Parent.Index);
			ChildHierarchy->Parent = NullEntity;
			return;
		}
		NewParent->Children.push_back(Child);
	}
	ChildHierarchy->Parent = Parent;
}

FEntity FScene::GetParent(FEntity Entity) const
{
	const FHierarchyComponent* Hierarchy = Registry.TryGet<FHierarchyComponent>(Entity);
	return Hierarchy ? Hierarchy->Parent : NullEntity;
}

const std::vector<FEntity>& FScene::GetChildren(FEntity Entity) const
{
	const FHierarchyComponent* Hierarchy = Registry.TryGet<FHierarchyComponent>(Entity);
	return Hierarchy ? Hierarchy->Children : GEmptyChildren;
}

bool FScene::IsAncestorOf(FEntity Ancestor, FEntity Entity) const
{
	FEntity Current = GetParent(Entity);
	while (Current.IsValid())
	{
		if (Current == Ancestor)
		{
			return true;
		}
		Current = GetParent(Current);
	}
	return false;
}

void FScene::UpdateTransforms()
{
	// 루트(부모 없음)부터 재귀적으로 갱신
	Registry.View<FTransformComponent, FHierarchyComponent>().Each(
		[this](FEntity Entity, FTransformComponent& Transform, FHierarchyComponent& Hierarchy) {
			if (!Hierarchy.Parent.IsValid())
			{
				Transform.WorldMatrix = Transform.GetLocalMatrix();
				for (FEntity Child : Hierarchy.Children)
				{
					UpdateTransformRecursive(Child, Transform.WorldMatrix);
				}
			}
			(void)Entity;
		});
}

void FScene::UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld)
{
	FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity);
	if (Transform == nullptr)
	{
		return;
	}

	// 행벡터 규약: Local * Parent
	Transform->WorldMatrix = Transform->GetLocalMatrix() * ParentWorld;

	if (const FHierarchyComponent* Hierarchy = Registry.TryGet<FHierarchyComponent>(Entity))
	{
		for (FEntity Child : Hierarchy->Children)
		{
			UpdateTransformRecursive(Child, Transform->WorldMatrix);
		}
	}
}

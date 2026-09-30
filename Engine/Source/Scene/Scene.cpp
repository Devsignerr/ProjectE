#include "Scene/Scene.h"

#include "Core/Log.h"
#include "Scene/ModelMetadata.h"
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

void FScene::Clear()
{
	std::vector<FEntity> Roots = GetRootEntities();
	for (FEntity Root : Roots)
	{
		DestroyEntity(Root);
	}
}

std::vector<FEntity> FScene::GetRootEntities() const
{
	std::vector<FEntity> Roots;
	if (const TSparseSet<FHierarchyComponent>* Pool = Registry.TryGetPool<FHierarchyComponent>())
	{
		const std::vector<FEntity>&             Entities   = Pool->GetEntities();
		const std::vector<FHierarchyComponent>& Components = Pool->GetComponents();
		for (size_t Index = 0; Index < Entities.size(); ++Index)
		{
			if (!Components[Index].Parent.IsValid())
			{
				Roots.push_back(Entities[Index]);
			}
		}
	}
	return Roots;
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
	// 루트(부모 없음)부터 재귀적으로 갱신. 소켓 부착 엔티티는 대상 모델의 뼈가 계산된 뒤로 미룬다
	DeferredAttachments.clear();
	Registry.View<FTransformComponent, FHierarchyComponent>().Each(
		[this](FEntity Entity, FTransformComponent& Transform, FHierarchyComponent& Hierarchy) {
			(void)Transform;
			if (!Hierarchy.Parent.IsValid())
			{
				UpdateTransformRecursive(Entity, FMatrix4x4::Identity, true);
			}
		});

	// 부착 처리: 대상(또는 대상의 조상)이 아직 처리되지 않은 부착 엔티티면 다음 차례로 (부착의 부착)
	for (int32 Round = 0; Round < 16 && !DeferredAttachments.empty(); ++Round)
	{
		std::vector<FEntity> Pending = std::move(DeferredAttachments);
		DeferredAttachments.clear();
		std::vector<FEntity> Waiting;
		for (const FEntity Entity : Pending)
		{
			const FEntity Target = Registry.Get<FSocketAttachmentComponent>(Entity).Target;
			const bool    bTargetPending = std::any_of(Pending.begin(), Pending.end(), [&](FEntity Other) {
                return Other != Entity && (Other == Target || IsAncestorOf(Other, Target));
			});
			if (bTargetPending && Round + 1 < 16)
			{
				Waiting.push_back(Entity);
				continue;
			}
			UpdateTransformRecursive(Entity, GetParentWorldMatrix(Entity), false);
		}
		DeferredAttachments.insert(DeferredAttachments.end(), Waiting.begin(), Waiting.end());
	}
	DeferredAttachments.clear();
}

void FScene::UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld, bool bAllowDefer)
{
	FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity);
	if (Transform == nullptr)
	{
		return;
	}
	if (bAllowDefer && IsSocketAttached(Entity))
	{
		DeferredAttachments.push_back(Entity);
		return;
	}

	// 행벡터 규약: Local * Parent
	Transform->WorldMatrix = Transform->GetLocalMatrix() * ParentWorld;

	if (const FHierarchyComponent* Hierarchy = Registry.TryGet<FHierarchyComponent>(Entity))
	{
		for (FEntity Child : Hierarchy->Children)
		{
			UpdateTransformRecursive(Child, Transform->WorldMatrix, true);
		}
	}
}

bool FScene::IsSocketAttached(FEntity Entity) const
{
	const FSocketAttachmentComponent* Attachment = Registry.TryGet<FSocketAttachmentComponent>(Entity);
	// 자기 자신이나 자기 하위(순환)를 대상으로 하면 무시한다
	return Attachment != nullptr && Registry.IsValid(Attachment->Target) && Attachment->Target != Entity && !IsAncestorOf(Entity, Attachment->Target);
}

bool FScene::GetSocketWorldMatrix(FEntity ModelRoot, std::string_view Socket, FMatrix4x4& OutWorld) const
{
	const FModelComponent* Model = Registry.IsValid(ModelRoot) ? Registry.TryGet<FModelComponent>(ModelRoot) : nullptr;
	if (Model == nullptr || !Model->Runtime.Metadata)
	{
		return false;
	}
	const FModelSocket* Found = Model->Runtime.Metadata->FindSocket(Socket);
	if (Found == nullptr)
	{
		return false;
	}
	FEntity Bone = ModelRoot;
	if (!Found->Bone.empty())
	{
		Bone = NullEntity;
		for (const FEntity Node : Model->Runtime.NodeEntities)
		{
			const FNameComponent* Name = Registry.IsValid(Node) ? Registry.TryGet<FNameComponent>(Node) : nullptr;
			if (Name != nullptr && Name->Name == Found->Bone)
			{
				Bone = Node;
				break;
			}
		}
		if (!Bone.IsValid())
		{
			return false;
		}
	}
	OutWorld = Found->GetLocalMatrix() * GetTransform(Bone).WorldMatrix;
	return true;
}

FMatrix4x4 FScene::GetParentWorldMatrix(FEntity Entity) const
{
	if (IsSocketAttached(Entity))
	{
		const FSocketAttachmentComponent& Attachment = Registry.Get<FSocketAttachmentComponent>(Entity);
		FMatrix4x4                        SocketWorld;
		if (GetSocketWorldMatrix(Attachment.Target, Attachment.Socket, SocketWorld))
		{
			return SocketWorld;
		}
	}
	const FEntity Parent = GetParent(Entity);
	return Parent.IsValid() ? GetTransform(Parent).WorldMatrix : FMatrix4x4::Identity;
}

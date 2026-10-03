#include "Scene/Scene.h"

#include "Core/Console/Console.h"
#include "Core/Jobs/ParallelFor.h"
#include "Core/Log.h"
#include "Core/Profiling.h"
#include "Scene/ModelMetadata.h"
#include "Scene/SceneReflection.h"

#include <algorithm>
#include <cstring>

E_DECLARE_LOG_CATEGORY(LogScene)
E_DEFINE_LOG_CATEGORY(LogScene, Log)

namespace
{
	const std::vector<FEntity> GEmptyChildren;

	TAutoConsoleVariable<bool> CVarTransformCache("scene.TransformCache", true,
	                                              "트랜스폼 갱신에서 로컬 TRS와 부모 월드가 지난 계산과 비트 단위로 같은 엔티티의 월드 행렬 계산을 건너뛴다 (0 = 매번 전체 계산, 결과는 같다)");

	template <typename T>
	bool BitEqual(const T& A, const T& B)
	{
		return std::memcmp(&A, &B, sizeof(T)) == 0;
	}

	// 월드 행렬 = 로컬 × 부모 월드. 입력(로컬 TRS, 부모 월드)이 마지막 계산과 비트 단위로 같으면 그대로 둔다 — 같은 입력이면 같은 결과이므로
	// 전체 재계산과 비트 동일하다. 로컬을 쓰는 쪽(스크립트·물리·애니메이션·복제·편집기)이 표시할 필요가 없다
	void UpdateWorldMatrix(FTransformComponent& Transform, const FMatrix4x4& ParentWorld, bool bUseCache)
	{
		FTransformComponent::FWorldCache& Cache = Transform.WorldCache;
		if (bUseCache && Cache.bValid && BitEqual(Cache.Position, Transform.Position) && BitEqual(Cache.Rotation, Transform.Rotation) &&
		    BitEqual(Cache.Scale, Transform.Scale) && BitEqual(Cache.ParentWorld, ParentWorld))
		{
			return;
		}
		// 행벡터 규약: Local * Parent
		Transform.WorldMatrix = Transform.GetLocalMatrix() * ParentWorld;
		Cache.Position        = Transform.Position;
		Cache.Rotation        = Transform.Rotation;
		Cache.Scale           = Transform.Scale;
		Cache.ParentWorld     = ParentWorld;
		Cache.bValid          = true;
	}
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
	E_PROFILE_SCOPE("트랜스폼 갱신");
	// 루트(부모 없음)부터 재귀적으로 갱신. 소켓 부착 엔티티는 대상 모델의 뼈가 계산된 뒤로 미룬다
	DeferredAttachments.clear();
	const TSparseSet<FSocketAttachmentComponent>* SocketPool = Registry.TryGetPool<FSocketAttachmentComponent>();
	TransformPool  = Registry.TryGetPool<FTransformComponent>();
	HierarchyPool  = Registry.TryGetPool<FHierarchyComponent>();
	bAnySockets    = SocketPool != nullptr && !SocketPool->IsEmpty();
	bUseWorldCache = CVarTransformCache.Get();
	if (TransformPool == nullptr || HierarchyPool == nullptr)
	{
		return;
	}
	// 1) 루트(부모 없음) 찾기를 병렬로: 엔티티 목록(View<트랜스폼, 계층>과 같은 목록·같은 순서 — 더 작은 풀, 같으면 트랜스폼)을 고정 크기 묶음으로
	//    나눠 묶음별 루트 목록을 만들고 묶음 순서대로 이어 붙인다 → 순차 순회와 같은 루트 순서 (View::Each보다 조회가 적어 순차로도 약 3배 빠름)
	const std::vector<FEntity>& Entities =
		HierarchyPool->Size() < TransformPool->Size() ? HierarchyPool->GetEntities() : TransformPool->GetEntities();
	constexpr uint32 ChunkSize  = 1024;
	const uint32     ChunkCount = static_cast<uint32>((Entities.size() + ChunkSize - 1) / ChunkSize);
	if (ChunkRoots.size() < ChunkCount)
	{
		ChunkRoots.resize(ChunkCount);
	}
	FParallel::ParallelFor(ChunkCount, 1, [this, &Entities](uint32 BeginChunk, uint32 EndChunk) {
		for (uint32 Chunk = BeginChunk; Chunk < EndChunk; ++Chunk)
		{
			std::vector<FEntity>& Roots = ChunkRoots[Chunk];
			Roots.clear();
			const size_t End = std::min<size_t>(Entities.size(), static_cast<size_t>(Chunk + 1) * ChunkSize);
			for (size_t Index = static_cast<size_t>(Chunk) * ChunkSize; Index < End; ++Index)
			{
				const FEntity              Entity    = Entities[Index];
				const FHierarchyComponent* Hierarchy = HierarchyPool->TryGet(Entity);
				if (Hierarchy != nullptr && !Hierarchy->Parent.IsValid() && TransformPool->Contains(Entity))
				{
					Roots.push_back(Entity);
				}
			}
		}
	});
	TransformRoots.clear();
	for (uint32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
	{
		TransformRoots.insert(TransformRoots.end(), ChunkRoots[Chunk].begin(), ChunkRoots[Chunk].end());
	}

	// 2) 루트 하위 트리는 서로 겹치지 않으므로 병렬로 갱신한다 (하위 트리의 월드 행렬은 그 루트를 맡은 스레드만 쓰고, 읽는 것은 계층·로컬 값뿐).
	//    미룬 부착 목록은 루트별로 모아 루트 순서대로 이어 붙인다 → 순차 갱신과 같은 순서·같은 결과
	const uint32 RootCount = static_cast<uint32>(TransformRoots.size());
	if (RootDeferred.size() < RootCount)
	{
		RootDeferred.resize(RootCount);
	}
	FParallel::ParallelFor(RootCount, 16, [this](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			RootDeferred[Index].clear();
			UpdateTransformRecursive(TransformRoots[Index], FMatrix4x4::Identity, true, RootDeferred[Index]);
		}
	});
	for (uint32 Index = 0; Index < RootCount; ++Index)
	{
		DeferredAttachments.insert(DeferredAttachments.end(), RootDeferred[Index].begin(), RootDeferred[Index].end());
	}

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
			UpdateTransformRecursive(Entity, GetParentWorldMatrix(Entity), false, DeferredAttachments);
		}
		DeferredAttachments.insert(DeferredAttachments.end(), Waiting.begin(), Waiting.end());
	}
	DeferredAttachments.clear();
}

void FScene::UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld, bool bAllowDefer, std::vector<FEntity>& OutDeferred)
{
	FTransformComponent* Transform = TransformPool->TryGet(Entity);
	if (Transform == nullptr)
	{
		return;
	}
	if (bAllowDefer && bAnySockets && IsSocketAttached(Entity))
	{
		OutDeferred.push_back(Entity);
		return;
	}

	UpdateWorldMatrix(*Transform, ParentWorld, bUseWorldCache);

	if (const FHierarchyComponent* Hierarchy = HierarchyPool != nullptr ? HierarchyPool->TryGet(Entity) : nullptr)
	{
		for (FEntity Child : Hierarchy->Children)
		{
			UpdateTransformRecursive(Child, Transform->WorldMatrix, true, OutDeferred);
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

#include "Scene/Scene.h"

#include "Core/Console/Console.h"
#include "Core/Jobs/ParallelFor.h"
#include "Core/Log.h"
#include "Core/Profiling.h"
#include "Scene/ModelMetadata.h"
#include "Scene/SceneReflection.h"

#include <algorithm>
#include <atomic>
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

	// 월드 행렬 계산 번호: 2부터 프로세스 전역에서 겹치지 않게 (묶음으로 받아 원자 연산 경합 없음). 1 = 루트의 부모(단위 행렬), 0 = 알 수 없음(항상 다시 계산)
	constexpr uint64    RootParentStamp    = 1;
	constexpr uint64    UnknownParentStamp = 0;
	std::atomic<uint64> GNextStampBlock{ 2 };
	constexpr uint64    StampBlockSize = 4096;

	struct FStampAllocator
	{
		uint64 Next = 0;
		uint64 End  = 0;

		uint64 Allocate()
		{
			if (Next == End)
			{
				Next = GNextStampBlock.fetch_add(StampBlockSize, std::memory_order_relaxed);
				End  = Next + StampBlockSize;
			}
			return Next++;
		}
	};
	thread_local FStampAllocator GThreadStamps; // 재귀 경로(소켓 부착 처리)용

	// 월드 행렬 = 로컬 × 부모 월드. 입력(로컬 TRS, 부모 월드의 계산 번호)이 마지막 계산과 비트 단위로 같으면 그대로 둔다 — 같은 번호는 같은
	// 부모 월드 값이므로 전체 재계산과 비트 동일하다. 로컬을 쓰는 쪽(스크립트·물리·애니메이션·복제·편집기)이 표시할 필요가 없다
	// 다시 계산했으면 true
	bool UpdateWorldMatrix(FTransformComponent& Transform, const FMatrix4x4& ParentWorld, uint64 ParentStamp, bool bUseCache, FStampAllocator& Stamps)
	{
		FTransformComponent::FWorldCache& Cache = Transform.WorldCache;
		const bool bLocalsSame =
			BitEqual(Cache.Position, Transform.Position) && BitEqual(Cache.Rotation, Transform.Rotation) && BitEqual(Cache.Scale, Transform.Scale);
		if (bUseCache && bLocalsSame && ParentStamp != UnknownParentStamp && Cache.ParentStamp == ParentStamp)
		{
			return false;
		}
		// 행벡터 규약: Local * Parent
		Transform.WorldMatrix = Transform.GetLocalMatrix() * ParentWorld;
		Transform.WorldStamp  = Stamps.Allocate();
		Cache.ParentStamp     = ParentStamp;
		if (!bLocalsSame) // 부모만 바뀐 경우(움직이는 캐릭터의 뼈 등)는 로컬 사본을 다시 쓰지 않는다 (그 캐시 줄을 더럽히지 않음)
		{
			Cache.Position = Transform.Position;
			Cache.Rotation = Transform.Rotation;
			Cache.Scale    = Transform.Scale;
		}
		return true;
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
	++HierarchyRevision;
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
	++HierarchyRevision;
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
	++HierarchyRevision;
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
	UpdateTransformsInternal(nullptr);
}

void FScene::UpdateTransformsPartial(std::span<const FTransformChangedSubtree> ChangedSinceLastUpdate)
{
	UpdateTransformsInternal(&ChangedSinceLastUpdate);
}

void FScene::UpdateTransformsInternal(const std::span<const FTransformChangedSubtree>* Changed)
{
	E_PROFILE_SCOPE("트랜스폼 갱신");
	// 루트(부모 없음)부터 부모 → 자식 순서로 갱신. 소켓 부착 엔티티는 대상 모델의 뼈가 계산된 뒤로 미룬다
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
	// 1) 갱신 계획(루트 순서 + 루트별 깊이 우선 평탄화)이 계층·풀 구조와 맞지 않으면 다시 만든다 — 구조가 그대로인 프레임은 루트 찾기·
	//    재귀·자식 목록 조회 없이 연속 배열을 앞에서부터 돈다 (부모 칸이 항상 자식보다 앞)
	// 부분 갱신은 직전 갱신 이후 구조가 그대로일 때만 (계획을 다시 만들어야 하면 새 엔티티가 있을 수 있으므로 전체)
	const bool bPartial = Changed != nullptr && bUseWorldCache && IsTransformPlanValid();
	if (!IsTransformPlanValid())
	{
		RebuildTransformPlan();
	}
	// 부분 갱신 표시: 바뀐 하위 트리 루트 칸 = 2, 그 조상 칸 = 1 (조상 사슬은 이미 표시된 칸에서 멈춘다)
	for (const uint32 Slot : PlanMarkedSlots)
	{
		PlanChangedMarks[Slot] = 0;
	}
	PlanMarkedSlots.clear();
	if (bPartial)
	{
		const auto FindSlot = [this](FEntity Entity) {
			if (!Entity.IsValid() || Entity.Index >= PlanSlotByEntityIndex.size())
			{
				return InvalidPlanSlot;
			}
			const uint32 Slot = PlanSlotByEntityIndex[Entity.Index];
			return Slot != InvalidPlanSlot && TransformPlanEntities[Slot] == Entity ? Slot : InvalidPlanSlot; // 트랜스폼이 없거나 지난 엔티티
		};
		const auto MarkSubtree = [this](uint32 Slot) {
			if (PlanChangedMarks[Slot] == 0)
			{
				PlanMarkedSlots.push_back(Slot);
			}
			PlanChangedMarks[Slot] = 2;
			for (Slot = TransformPlan[Slot].ParentSlot; Slot != InvalidPlanSlot && PlanChangedMarks[Slot] == 0; Slot = TransformPlan[Slot].ParentSlot)
			{
				PlanChangedMarks[Slot] = 1;
				PlanMarkedSlots.push_back(Slot);
			}
		};
		for (const FTransformChangedSubtree& Change : *Changed)
		{
			const uint32 RootSlot = FindSlot(Change.Root);
			if (RootSlot != InvalidPlanSlot)
			{
				MarkSubtree(RootSlot);
			}
			// 함께 바뀐 엔티티(모델 노드)가 그 하위 트리 밖이면 (계층을 옮긴 뼈 등) 따로 표시. 모두 안이라고 확인한 목록(같은 배열)은
			// 계획이 그대로인 동안 다시 확인하지 않는다 (노드 목록이 바뀌는 모델 재생성은 엔티티를 만들어 계획을 다시 만든다)
			if (Change.Members.empty())
			{
				continue;
			}
			if (RootSlot != InvalidPlanSlot)
			{
				const auto Found = PlanVerifiedMembers.find(RootSlot);
				if (Found != PlanVerifiedMembers.end() && Found->second.first == Change.Members.data() && Found->second.second == Change.Members.size())
				{
					continue;
				}
			}
			const uint32 End     = RootSlot != InvalidPlanSlot ? TransformPlan[RootSlot].SubtreeEnd : 0;
			bool         bInside = RootSlot != InvalidPlanSlot;
			for (const FEntity Member : Change.Members)
			{
				const uint32 Slot = FindSlot(Member);
				if (Slot != InvalidPlanSlot && (RootSlot == InvalidPlanSlot || Slot < RootSlot || Slot >= End))
				{
					MarkSubtree(Slot);
					bInside = false;
				}
			}
			if (bInside)
			{
				PlanVerifiedMembers[RootSlot] = { Change.Members.data(), Change.Members.size() };
			}
		}
	}

	// 2) 소켓 부착 엔티티 표시 (부착 컴포넌트를 가진 엔티티만 — 계획 밖에서 바뀔 수 있으므로 매번)
	for (const uint32 Slot : PlanSocketSlots)
	{
		PlanSocketFlags[Slot] = 0;
	}
	PlanSocketSlots.clear();
	if (bAnySockets)
	{
		for (const FEntity Entity : SocketPool->GetEntities())
		{
			if (Entity.Index < PlanSlotByEntityIndex.size())
			{
				const uint32 Slot = PlanSlotByEntityIndex[Entity.Index];
				if (Slot != InvalidPlanSlot && TransformPlanEntities[Slot] == Entity && IsSocketAttached(Entity))
				{
					PlanSocketFlags[Slot] = 1;
					PlanSocketSlots.push_back(Slot);
				}
			}
		}
	}

	// 3) 루트 하위 트리는 서로 겹치지 않으므로 병렬로 갱신한다 (하위 트리의 월드 행렬은 그 루트를 맡은 스레드만 쓰고, 읽는 것은 로컬 값과 부모 칸뿐).
	//    미룬 부착 목록은 루트별로 모아 루트 순서대로 이어 붙인다 → 순차 갱신과 같은 순서·같은 결과
	const uint32 RootCount = static_cast<uint32>(TransformPlanRootStarts.size()) - 1;
	if (RootDeferred.size() < RootCount)
	{
		RootDeferred.resize(RootCount);
	}
	FParallel::ParallelFor(RootCount, 16, [this, bPartial](uint32 Begin, uint32 End) {
		std::vector<FTransformComponent>& Components = TransformPool->GetComponents();
		const bool                        bSockets   = !PlanSocketSlots.empty();
		FStampAllocator                   Stamps;
		// 하위 트리를 건너뛸 때도 그 안의 소켓 부착 엔티티는 미룬다 (대상 모델은 다른 하위 트리일 수 있다)
		const auto DeferSocketsIn = [&](uint32 From, uint32 To, std::vector<FEntity>& Deferred) {
			for (uint32 Slot = From; Slot < To;)
			{
				if (PlanSocketFlags[Slot] != 0)
				{
					Deferred.push_back(TransformPlanEntities[Slot]);
					Slot = TransformPlan[Slot].SubtreeEnd;
					continue;
				}
				++Slot;
			}
		};
		for (uint32 Root = Begin; Root < End; ++Root)
		{
			std::vector<FEntity>& Deferred = RootDeferred[Root];
			Deferred.clear();
			const uint32 SegmentEnd = TransformPlanRootStarts[Root + 1];
			for (uint32 Slot = TransformPlanRootStarts[Root]; Slot < SegmentEnd;)
			{
				const FTransformPlanEntry& Entry = TransformPlan[Slot];
				if (bSockets && PlanSocketFlags[Slot] != 0)
				{
					Deferred.push_back(TransformPlanEntities[Slot]); // 하위 트리째 미룬다 (부착 처리가 재귀로 갱신)
					Slot = Entry.SubtreeEnd;
					continue;
				}
				// 부분 갱신: 바뀐 엔티티가 없는 하위 트리이고 부모가 다시 계산되지도, 바뀐 하위 트리 안도 아니면 입력이 모두 그대로
				// → 전체 갱신이었다면 모두 캐시 적중 (결과 동일)
				const uint8 ParentState = bPartial && Entry.ParentSlot != InvalidPlanSlot ? PlanSlotState[Entry.ParentSlot] : uint8(0);
				if (bPartial && PlanChangedMarks[Slot] == 0 && ParentState == 0)
				{
					if (bSockets)
					{
						DeferSocketsIn(Slot + 1, Entry.SubtreeEnd, Deferred);
					}
					Slot = Entry.SubtreeEnd;
					continue;
				}
				FTransformComponent& Transform = Components[Entry.Dense];
				bool                 bRecomputed;
				if (Entry.ParentSlot == InvalidPlanSlot)
				{
					bRecomputed = UpdateWorldMatrix(Transform, FMatrix4x4::Identity, RootParentStamp, bUseWorldCache, Stamps);
				}
				else
				{
					const FTransformComponent& Parent = Components[TransformPlan[Entry.ParentSlot].Dense];
					bRecomputed = UpdateWorldMatrix(Transform, Parent.WorldMatrix, Parent.WorldStamp, bUseWorldCache, Stamps);
				}
				if (bPartial)
				{
					// bit0 = 다시 계산함, bit1 = 바뀐 하위 트리 안 (자식이 따라 돈다)
					const bool bInChanged = PlanChangedMarks[Slot] == 2 || (ParentState & 2) != 0;
					PlanSlotState[Slot]   = static_cast<uint8>((bRecomputed ? 1 : 0) | (bInChanged ? 2 : 0));
				}
				++Slot;
			}
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
			UpdateTransformRecursive(Entity, GetParentWorldMatrix(Entity), UnknownParentStamp, false, DeferredAttachments); // 소켓 기준: 항상 다시 계산
		}
		DeferredAttachments.insert(DeferredAttachments.end(), Waiting.begin(), Waiting.end());
	}
	DeferredAttachments.clear();
}

bool FScene::IsTransformPlanValid() const
{
	return PlanTransformPool == TransformPool && PlanHierarchyPool == HierarchyPool && PlanTransformPoolRevision == TransformPool->GetRevision() &&
	       PlanHierarchyPoolRevision == HierarchyPool->GetRevision() && PlanHierarchyRevision == HierarchyRevision && !TransformPlanRootStarts.empty();
}

void FScene::RebuildTransformPlan()
{
	E_PROFILE_SCOPE("트랜스폼 갱신 계획");
	// 루트(부모 없음) 찾기: 엔티티 목록(View<트랜스폼, 계층>과 같은 목록·같은 순서 — 더 작은 풀, 같으면 트랜스폼) 순서 = 이전 재귀 갱신의 루트 순서
	const std::vector<FEntity>& Entities =
		HierarchyPool->Size() < TransformPool->Size() ? HierarchyPool->GetEntities() : TransformPool->GetEntities();
	TransformPlan.clear();
	TransformPlanEntities.clear();
	TransformPlanRootStarts.clear();
	TransformPlan.reserve(TransformPool->Size());
	TransformPlanEntities.reserve(TransformPool->Size());
	std::fill(PlanSlotByEntityIndex.begin(), PlanSlotByEntityIndex.end(), InvalidPlanSlot);

	// 깊이 우선 전위 순서 (자식 목록 순서) — 재귀 갱신과 같은 방문 순서. 트랜스폼이 없는 엔티티는 하위 트리째 뺀다
	struct FStackItem
	{
		FEntity Entity;
		uint32  ParentSlot;
	};
	std::vector<FStackItem> Stack;
	std::vector<uint32>     Open; // 하위 트리 끝을 아직 정하지 않은 칸 (조상 사슬)
	for (const FEntity Root : Entities)
	{
		const FHierarchyComponent* RootHierarchy = HierarchyPool->TryGet(Root);
		if (RootHierarchy == nullptr || RootHierarchy->Parent.IsValid() || !TransformPool->Contains(Root))
		{
			continue;
		}
		TransformPlanRootStarts.push_back(static_cast<uint32>(TransformPlan.size()));
		Stack.push_back({ Root, InvalidPlanSlot });
		while (!Stack.empty())
		{
			const FStackItem Item = Stack.back();
			Stack.pop_back();
			// 이 칸의 부모가 아닌 열린 칸(이미 끝난 형제 하위 트리)을 닫는다
			while (!Open.empty() && Open.back() != Item.ParentSlot)
			{
				TransformPlan[Open.back()].SubtreeEnd = static_cast<uint32>(TransformPlan.size());
				Open.pop_back();
			}
			if (!TransformPool->Contains(Item.Entity))
			{
				continue;
			}
			const uint32 Slot = static_cast<uint32>(TransformPlan.size());
			TransformPlan.push_back({ TransformPool->GetDenseIndex(Item.Entity), Item.ParentSlot, Slot + 1 });
			TransformPlanEntities.push_back(Item.Entity);
			if (Item.Entity.Index >= PlanSlotByEntityIndex.size())
			{
				PlanSlotByEntityIndex.resize(static_cast<size_t>(Item.Entity.Index) + 1, InvalidPlanSlot);
			}
			PlanSlotByEntityIndex[Item.Entity.Index] = Slot;
			Open.push_back(Slot);
			if (const FHierarchyComponent* Hierarchy = HierarchyPool->TryGet(Item.Entity))
			{
				for (auto It = Hierarchy->Children.rbegin(); It != Hierarchy->Children.rend(); ++It)
				{
					Stack.push_back({ *It, Slot });
				}
			}
		}
		while (!Open.empty())
		{
			TransformPlan[Open.back()].SubtreeEnd = static_cast<uint32>(TransformPlan.size());
			Open.pop_back();
		}
	}
	TransformPlanRootStarts.push_back(static_cast<uint32>(TransformPlan.size()));
	PlanSocketFlags.assign(TransformPlan.size(), 0);
	PlanSocketSlots.clear();
	PlanChangedMarks.assign(TransformPlan.size(), 0);
	PlanMarkedSlots.clear();
	PlanSlotState.assign(TransformPlan.size(), 0);
	PlanVerifiedMembers.clear();

	PlanTransformPool         = TransformPool;
	PlanHierarchyPool         = HierarchyPool;
	PlanTransformPoolRevision = TransformPool->GetRevision();
	PlanHierarchyPoolRevision = HierarchyPool->GetRevision();
	PlanHierarchyRevision     = HierarchyRevision;
}

void FScene::UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld, uint64 ParentStamp, bool bAllowDefer, std::vector<FEntity>& OutDeferred)
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

	UpdateWorldMatrix(*Transform, ParentWorld, ParentStamp, bUseWorldCache, GThreadStamps);

	if (const FHierarchyComponent* Hierarchy = HierarchyPool != nullptr ? HierarchyPool->TryGet(Entity) : nullptr)
	{
		for (FEntity Child : Hierarchy->Children)
		{
			UpdateTransformRecursive(Child, Transform->WorldMatrix, Transform->WorldStamp, true, OutDeferred);
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

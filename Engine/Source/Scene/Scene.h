#pragma once

#include "Core/ECS/Registry.h"
#include "Scene/Components.h"

#include <span>
#include <unordered_map>
#include <utility>
#include <string_view>

// 부분 트랜스폼 갱신 항목 (FScene::UpdateTransformsPartial): Root와 그 하위 트리 전체 + Members(보통 Root 하위 트리 안 — 밖이면 따로 본다)
struct FTransformChangedSubtree
{
	FEntity                  Root;
	std::span<const FEntity> Members;
};

// 씬: 레지스트리 소유 + 계층/트랜스폼 편의 기능
class FScene
{
public:
	FScene(); // 컴포넌트 리플렉션 등록 보장

	FRegistry&       GetRegistry() { return Registry; }
	const FRegistry& GetRegistry() const { return Registry; }

	// 이름/트랜스폼/계층 컴포넌트를 갖춘 엔티티 생성
	FEntity CreateEntity(std::string_view Name = "Entity");

	// 자식까지 재귀 파괴
	void DestroyEntity(FEntity Entity);

	// 모든 엔티티 파괴
	void Clear();

	// 루트 엔티티(부모 없음) 목록을 생성 순서(풀 순서)대로 반환
	std::vector<FEntity> GetRootEntities() const;

	// Parent가 NullEntity면 루트로 만든다. 순환은 거부한다(로그 후 무시).
	void SetParent(FEntity Child, FEntity Parent);

	FEntity                     GetParent(FEntity Entity) const;
	const std::vector<FEntity>& GetChildren(FEntity Entity) const;
	bool                        IsAncestorOf(FEntity Ancestor, FEntity Entity) const;

	// 모든 엔티티의 WorldMatrix를 부모→자식 순서로 갱신. 소켓 부착 엔티티는 대상 모델 뒤에 소켓 기준으로
	void UpdateTransforms();
	// 부분 갱신 — 호출자 보장: 직전 UpdateTransforms(전체·부분) 이후 로컬 트랜스폼을 썼을 수 있는 엔티티는 Changed 항목(Root와 그 하위 트리 전체,
	// Members)뿐이고 WorldMatrix를 직접 쓴 곳도 없다. 바뀐 하위 트리·그 조상 사슬·다시 계산된 엔티티의 하위 트리만 돌고 나머지는 건너뛴다
	// (전체 갱신이었다면 모두 캐시 적중 — 결과 비트 동일, Scene_PartialTransformUpdateMatchesFull). 계층·풀 구조가 바뀌었거나 캐시가 꺼져 있으면
	// 전체 갱신. 소켓 부착 엔티티는 항상 다시 계산한다.
	// 쓰는 곳: FGameWorld::TickPresentation (게임플레이 틱의 전체 갱신 직후 — 사이에 쓰는 것은 시간대 태양과 애니메이션이 평가한 모델뿐)
	void UpdateTransformsPartial(std::span<const FTransformChangedSubtree> ChangedSinceLastUpdate);

	// 모델 루트의 소켓(.emeta) 월드 행렬 = 소켓 로컬 × 뼈 월드 (직전 UpdateTransforms 기준). 소켓/뼈가 없으면 false
	bool GetSocketWorldMatrix(FEntity ModelRoot, std::string_view Socket, FMatrix4x4& OutWorld) const;
	// 로컬 ↔ 월드 변환 기준 (소켓 부착이면 소켓, 아니면 계층 부모, 루트면 단위 행렬). 기즈모/물리가 로컬 값을 쓸 때 사용
	FMatrix4x4 GetParentWorldMatrix(FEntity Entity) const;
	// 유효한 대상(자기 하위가 아닌 엔티티)에 소켓 부착되어 있는지
	bool IsSocketAttached(FEntity Entity) const;

	// 계층 컴포넌트를 SetParent 밖에서 직접 고친 코드(FSceneCloner)가 부른다 — 트랜스폼 갱신 계획을 다시 만들게 한다
	void NotifyHierarchyChanged() { ++HierarchyRevision; }
	// SetParent/생성/파괴마다 바뀐다 — 계층에서 파생한 캐시(렌더러 LOD 모델 묶음 등)의 무효화 기준
	uint64 GetHierarchyRevision() const { return HierarchyRevision; }

	FTransformComponent&       GetTransform(FEntity Entity) { return Registry.Get<FTransformComponent>(Entity); }
	const FTransformComponent& GetTransform(FEntity Entity) const { return Registry.Get<FTransformComponent>(Entity); }

private:
	// bAllowDefer면 소켓 부착 엔티티를 OutDeferred에 미룬다 (루트 하위 트리별 목록 — 병렬 갱신)
	void UpdateTransformRecursive(FEntity Entity, const FMatrix4x4& ParentWorld, uint64 ParentStamp, bool bAllowDefer, std::vector<FEntity>& OutDeferred);

	// 갱신 계획: 루트 순서(풀 순서) + 루트별 깊이 우선 전위 평탄화. 계층(SetParent/생성/파괴)이나 트랜스폼·계층 풀 구조가 바뀌면 다시 만든다
	struct FTransformPlanEntry
	{
		uint32 Dense;      // 트랜스폼 풀 밀집 인덱스
		uint32 ParentSlot; // 부모 칸 (InvalidPlanSlot = 루트)
		uint32 SubtreeEnd; // 이 칸 하위 트리 다음 칸
	};
	static constexpr uint32 InvalidPlanSlot = ~0u;
	bool IsTransformPlanValid() const;
	void RebuildTransformPlan();
	void UpdateTransformsInternal(const std::span<const FTransformChangedSubtree>* Changed);

	FRegistry                         Registry;
	// UpdateTransforms 동안만 유효 (재귀에서 풀 조회를 매번 하지 않도록)
	TSparseSet<FTransformComponent>*       TransformPool  = nullptr;
	const TSparseSet<FHierarchyComponent>* HierarchyPool  = nullptr;
	bool                                   bAnySockets    = false; // 소켓 부착 컴포넌트가 하나라도 있으면 (없으면 부착 판정 생략)
	bool                                   bUseWorldCache = true;  // scene.TransformCache
	std::vector<FEntity>              DeferredAttachments; // UpdateTransforms 작업 목록
	std::vector<std::vector<FEntity>> RootDeferred;        // 루트별 미룬 부착 엔티티 (루트 순서대로 이어 붙임)

	uint64                           HierarchyRevision = 1; // SetParent/생성/파괴/NotifyHierarchyChanged마다 증가
	std::vector<FTransformPlanEntry> TransformPlan;
	std::vector<FEntity>             TransformPlanEntities;   // 칸 → 엔티티
	std::vector<uint32>              TransformPlanRootStarts; // 루트별 시작 칸 (+ 끝 = 전체 칸 수)
	std::vector<uint32>              PlanSlotByEntityIndex;   // 엔티티 인덱스 → 칸
	std::vector<uint8>               PlanSocketFlags;         // 이번 갱신에서 미룰 소켓 부착 칸
	std::vector<uint32>              PlanSocketSlots;         // PlanSocketFlags를 켠 칸 (다음 갱신에서 끔)
	std::vector<uint8>               PlanChangedMarks;        // 부분 갱신: 2 = 바뀐 하위 트리 루트, 1 = 그 조상
	std::vector<uint32>              PlanMarkedSlots;         // PlanChangedMarks를 켠 칸 (다음 갱신에서 끔)
	std::vector<uint8>               PlanSlotState;           // 부분 갱신: 이번 갱신에서 돈 칸의 bit0 다시 계산, bit1 바뀐 하위 트리 안
	std::unordered_map<uint32, std::pair<const FEntity*, size_t>> PlanVerifiedMembers; // 부분 갱신: 하위 트리 안으로 확인한 Members (루트 칸 → 배열)
	const void*                      PlanTransformPool         = nullptr;
	const void*                      PlanHierarchyPool         = nullptr;
	uint64                           PlanTransformPoolRevision = 0;
	uint64                           PlanHierarchyPoolRevision = 0;
	uint64                           PlanHierarchyRevision     = 0;
};

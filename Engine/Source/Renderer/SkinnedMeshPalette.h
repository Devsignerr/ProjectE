#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "Scene/ResourceHandles.h"

#include <functional>
#include <vector>

class FD3D12DynamicUploadBuffer;
class FResourceManager;
class FScene;
class FStaticMesh;
struct FSkinComponent;

// 스킨 메시 엔티티 하나의 이번 프레임 드로우 정보
struct FSkinnedDrawInfo
{
	uint32 BoneOffset = 0; // 프레임 팔레트 버퍼(SkinnedMesh.hlsli SkinBones, t15) 안 첫 본 행렬 번호 (인스턴스 데이터 BoneOffset).
	                       // 같은 스킨(조인트 목록 + 역바인드 행렬이 같은 프리미티브들)은 같은 범위를 공유한다
	uint32 BoneCount  = 0;
	uint32 PrevBoneOffset = 0; // 같은 버퍼 안 이전 프레임 팔레트 첫 본 (움직임 벡터). 이력이 없으면 BoneOffset
	FBox   WorldBounds;    // 조인트별 바인드 경계를 팔레트로 변환한 합집합 (보수적)
};

// 프레임마다 보이는 스킨 메시의 본 팔레트를 하나의 구조화 버퍼(float4x4 배열)로 동적 업로드 버퍼에 한 번 올리고,
// 모든 스킨 패스(메인/방향광 그림자/로컬 그림자/아웃라인)가 인스턴스 데이터의 BoneOffset으로 인덱싱한다.
// 가시성 (Build의 IsVisible): 팔레트를 계산하기 전에 조인트 위치 + 조인트별 반경(바인드 경계 꼭짓점의 조인트 공간 거리 최댓값,
//   엔티티별 캐시)으로 만든 보수적 경계를 판정한다. 호출자는 "메인 프러스텀 ∪ 그림자 캐스터 볼륨(CSM 캐스케이드, 로컬 그림자 장)"을
//   넘긴다 → 화면 밖이지만 화면 안으로 그림자를 드리우는 캐릭터도 팔레트가 나온다. 통과한 것만 팔레트와 정확한 경계를 계산한다.
//   통과하지 못한 엔티티는 IsCulled = true이며 그 프레임 어떤 패스에도 나오지 않는다 (정적 메시로 그려지면 안 된다).
// 팔레트 공유: 한 모델의 여러 프리미티브 엔티티는 각자 FSkinComponent를 갖지만 조인트 엔티티 목록과 역바인드 행렬이 같으면
//   팔레트 하나(같은 BoneOffset/PrevBoneOffset)를 쓴다 (그룹 = 이번 프레임 처음 나온 엔티티가 대표). 가시성·경계는 프리미티브마다.
//   이전 프레임 팔레트는 대표 엔티티 칸에 두고, 구성원은 바로 앞 Build에서도 같은 대표의 그룹에서 팔레트가 계산됐을 때만 이력이 있다.
// 계산 (모두 결정적 — 결과는 실행 순서와 무관, IsVisible은 여러 스레드에서 불린다 — 읽기 전용이어야 한다):
//   1) 후보 조회(병렬, 뷰 순서 그대로 칸마다: 메시 해석 + 조인트 목록 키) → 2) 그룹 묶기(호출 스레드, 조회 결과만 읽고 키로 비교) →
//      그룹 평가(병렬: 키로 묶은 구성원을 IsSameSkin으로 확인 — 키 충돌이면 정확 비교로 다시 묶고 다시 평가, 조인트 월드 행렬/가시성/이력 판정) →
//   3) 업로드 자리·드로우 번호(호출 스레드) → 4) 그룹마다 병렬: 팔레트/경계(ComputeSkinnedBounds)/업로드 버퍼에 직접 쓰기/이력/드로우 정보
// 호출: FScene::UpdateTransforms 이후, 그림자 캐스터 볼륨(캐스케이드/로컬 그림자 장)을 정한 뒤, 메시 인스턴스 Gather 전에 Build
class FSkinnedMeshPalette
{
public:
	using FVisibilityTest = std::function<bool(const FBox&)>;

	// IsVisible이 비어 있으면 모두 보이는 것으로 (여러 스레드에서 동시에 불린다)
	void Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer, const FVisibilityTest& IsVisible = {});

	// 이전 프레임 팔레트도 올린다 (움직임 벡터). 엔티티가 바로 앞 Build에서도 팔레트가 계산됐고 본 수가 같을 때만 이력이 있다.
	// 버퍼 = [이번 프레임 팔레트들][이전 프레임 팔레트들]. 한 렌더러가 여러 씬을 번갈아 그리면 엔티티 번호가 겹쳐 이력이 틀릴 수 있다
	// (씬 렌더러는 그때 시간 누적 효과를 끈다)
	bool bTrackPrevious = true;

	// 스킨으로 그릴 엔티티면 정보, 아니면 nullptr (FSkinComponent + 스킨 스트림 메시가 모두 있어야 함)
	const FSkinnedDrawInfo* Find(FEntity Entity) const;
	// 스킨 메시인데 이번 프레임 가시성 판정에서 빠진 엔티티 (그리지 않는다)
	bool IsCulled(FEntity Entity) const;

	// 프레임 팔레트 구조화 버퍼 (루트 SRV t15). Build한 프레임 안에서만 유효. 비어 있어도 유효한 주소
	D3D12_GPU_VIRTUAL_ADDRESS GetGpuData() const { return GpuData; }

	size_t GetCount() const { return Draws.size(); } // 팔레트로 그리는 스킨 메시 엔티티(프리미티브) 수
	uint32 GetPaletteCount() const { return PaletteCount; } // 이번 프레임 계산한 팔레트 수 (공유 그룹)
	uint32 GetCulledCount() const { return CulledCount; }
	uint32 GetBoneCount() const { return BoneCount; }

	// 팔레트 계산 (테스트용 공개): Palette[i] = InverseBind[i] * JointWorld[i]
	static void ComputePalette(const std::vector<FMatrix4x4>& InverseBindMatrices, const std::vector<FMatrix4x4>& JointWorldMatrices,
	                           std::vector<FMatrix4x4>& OutPalette);
	// 조인트별 반경 (테스트용 공개): 바인드 경계 꼭짓점을 InverseBind[i]로 옮긴 점의 원점 거리 최댓값 (조인트 로컬 단위)
	static void ComputeJointRadii(const FBox& LocalBounds, const std::vector<FMatrix4x4>& InverseBindMatrices, std::vector<float>& OutRadii);
	// 보수적 월드 경계 (테스트용 공개): 조인트 위치들의 AABB ± max_i(Radii[i] × 조인트 3x3의 프로베니우스 노름 — 최대 특이값의 상한)
	static FBox ComputeConservativeBounds(const std::vector<FMatrix4x4>& JointWorldMatrices, const std::vector<float>& Radii);
	// 스킨 메시 월드 경계 (테스트용 공개): 바인드 경계 상자를 팔레트 행렬마다 옮긴 AABB의 합집합.
	// FBox::AddBox(LocalBounds를 행렬로 옮긴 상자 — 중심·반 크기 식)를 뼈 순서대로 반복한 것과 비트 단위로 같다 (SSE로 열을 함께 계산)
	static FBox ComputeSkinnedBounds(const FBox& LocalBounds, const FMatrix4x4* Palette, uint32 Count);

private:
	// 엔티티 인덱스별 칸 (해시 대신 배열 — 세대로 검증): 프레임을 넘는 캐시·이력
	struct FEntitySlot
	{
		// 조인트 반경 캐시
		const FStaticMesh* Mesh        = nullptr;
		const FMatrix4x4*  InverseBind = nullptr; // FSkinComponent::InverseBindMatrices.data() (바뀌면 다시 계산)
		size_t             Count       = 0;
		uint32             RadiiGeneration = ~0u;
		std::vector<float> Radii;
		// 팔레트 공유 판정 캐시: 대표의 역바인드 행렬과 내용이 같다고 확인한 (대표, 자기) 배열 주소·크기 (같으면 비교 생략)
		const FMatrix4x4* SharedLeaderInverseBind = nullptr;
		const FMatrix4x4* SharedOwnInverseBind    = nullptr;
		size_t            SharedCount             = 0;
		// 이전 프레임 팔레트 (bTrackPrevious)
		uint64                  PaletteBuild      = 0;     // 이 엔티티가 팔레트로 그려진 마지막 Build
		uint32                  PaletteGeneration = ~0u;
		FEntity                 PaletteLeader;             // 그때 그룹 대표
		uint64                  PrevPaletteBuild  = 0;     // 대표로서 팔레트를 남긴 마지막 Build
		uint32                  PrevPaletteOffset = 0;     // 그 Build의 GroupBones(지금은 PrevGroupBones) 안 자리
		uint32                  PrevPaletteCount  = 0;
	};
	// 엔티티 인덱스별 이번 Build 결과 (Find/IsCulled — 메시 인스턴스 수집이 엔티티마다 읽으므로 작게)
	struct FEntityDraw
	{
		uint32 Generation = 0;
		uint32 Build      = 0;  // 채운 Build 번호 (하위 32비트)
		int32  Draw       = -1; // Draws 번호 (-1 = 이번 프레임 컬링됨)
	};
	// 뷰 순회 칸 하나 (병렬 조회 결과). Mesh가 nullptr이면 후보 아님
	struct FLookup
	{
		FEntity               Entity;
		FEntity               RootJoint; // 첫 조인트
		const FSkinComponent* Skin     = nullptr;
		const FStaticMesh*    Mesh     = nullptr;
		uint64                JointKey = 0; // 조인트 목록 + 역바인드 수 해시 (그룹 묶기 1차 비교)
		uint32                JointsSize      = 0;
		uint32                InverseBindSize = 0;
		bool                  bMeshVisible = false;
	};
	// 이번 Build의 스킨 메시 엔티티 하나
	struct FCandidate
	{
		FEntity               Entity;
		const FSkinComponent* Skin = nullptr;
		const FStaticMesh*    Mesh = nullptr;
		FBox                  WorldBounds;        // 보일 때 팔레트 기준 경계
		uint64                JointKey   = 0;
		uint32                JointsSize      = 0;
		uint32                InverseBindSize = 0;
		uint32                Group      = 0;
		int32                 NextMember = -1;    // 같은 그룹의 다음 후보
		int32                 Draw       = -1;    // Draws 번호 (보일 때)
		bool                  bTestVisibility = false;
		bool                  bVisible        = false;
		bool                  bVerify         = false; // 키로 그룹에 들어감 — IsSameSkin으로 확인 필요
	};
	// 팔레트 공유 그룹 (같은 조인트 목록 + 역바인드 행렬)
	struct FGroup
	{
		uint32 Leader       = 0;   // Candidates 번호 (처음 나온 엔티티 = 첫 구성원)
		int32  LastMember   = -1;
		uint32 JointOffset  = 0;   // GroupBones 안 첫 조인트
		uint32 JointCount   = 0;
		uint32 PaletteCount = 0;   // min(역바인드 수, JointCount)
		uint32 BoneOffset   = 0;   // 업로드 버퍼 안 첫 본 (보일 때)
		uint32 PrevOffset   = ~0u; // 업로드 버퍼 이전 프레임 영역 안 첫 본 (이력 없으면 ~0)
		int32  NextSameRoot = -1;  // 첫 조인트가 같은 다음 그룹
		bool   bVisible     = false;
		bool   bHasPrev     = false; // 대표가 바로 앞 Build에서도 대표로 팔레트를 남겼다 (PrevGroupBones의 대표 칸 자리를 업로드)
	};
	const FEntityDraw* FindDraw(FEntity Entity) const;
	bool               IsSameSkin(const FCandidate& Candidate, const FGroup& Group);
	// Lookups(뷰 순서)의 후보를 그룹으로 묶는다. bExact = IsSameSkin으로 바로 비교, 아니면 조인트 키로 묶고 bVerify 표시
	void               GroupCandidates(const FVisibilityTest& IsVisible, bool bExact);
	// 그룹마다 조인트 월드 행렬 읽기 + 구성원 가시성 + 이력 판정 (병렬). 키로 묶은 구성원을 IsSameSkin으로 확인해 하나라도 틀리면 false
	bool               EvaluateGroups(const FScene& Scene, const FVisibilityTest& IsVisible);

	std::vector<FSkinnedDrawInfo> Draws;
	std::vector<FEntitySlot>      Slots;
	std::vector<FEntityDraw>      EntityDraws;
	uint32                        CulledCount  = 0;
	uint32                        PaletteCount = 0;
	uint32                        BoneCount    = 0;
	std::vector<FCandidate>                      Candidates;
	std::vector<FGroup>                          Groups;
	std::vector<FLookup>                         Lookups;
	std::vector<std::pair<uint64, int32>>        GroupByRootJoint; // 첫 조인트 엔티티 인덱스 → (묶기 번호, 첫 그룹)
	uint64                                       GroupPassCount = 0;
	std::vector<FMatrix4x4>                      GroupBones;     // 그룹별 조인트 월드 행렬 → 병렬 단계에서 제자리 팔레트로 (이어 붙임, 줄이지 않음 — 매번 덮어씀)
	std::vector<FMatrix4x4>                      PrevGroupBones; // 바로 앞 Build의 GroupBones (Build마다 맞바꿈 — 이전 프레임 팔레트를 복사해 두지 않는다)
	std::vector<float>                           GroupJointScale; // 그룹별 조인트 3x3 프로베니우스 노름 제곱 (보수적 경계)
	uint32                                       GroupJointTotal = 0; // 이번 묶기의 조인트 수 합
	D3D12_GPU_VIRTUAL_ADDRESS                    GpuData    = 0;
	uint64                                       BuildCount = 0;
};

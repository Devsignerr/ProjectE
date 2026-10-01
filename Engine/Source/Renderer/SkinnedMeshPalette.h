#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "Scene/ResourceHandles.h"

#include <functional>
#include <vector>

class FD3D12DynamicUploadBuffer;
class FResourceManager;
class FScene;
class FStaticMesh;
struct FEntity;

// 스킨 메시 엔티티 하나의 이번 프레임 드로우 정보
struct FSkinnedDrawInfo
{
	uint32 BoneOffset = 0; // 프레임 팔레트 버퍼(SkinnedMesh.hlsli SkinBones, t15) 안 첫 본 행렬 번호 (인스턴스 데이터 BoneOffset)
	uint32 BoneCount  = 0;
	FBox   WorldBounds;    // 조인트별 바인드 경계를 팔레트로 변환한 합집합 (보수적)
};

// 프레임마다 보이는 스킨 메시의 본 팔레트를 하나의 구조화 버퍼(float4x4 배열)로 동적 업로드 버퍼에 한 번 올리고,
// 모든 스킨 패스(메인/방향광 그림자/로컬 그림자/아웃라인)가 인스턴스 데이터의 BoneOffset으로 인덱싱한다.
// 가시성 (Build의 IsVisible): 팔레트를 계산하기 전에 조인트 위치 + 조인트별 반경(바인드 경계 꼭짓점의 조인트 공간 거리 최댓값,
//   엔티티별 캐시)으로 만든 보수적 경계를 판정한다. 호출자는 "메인 프러스텀 ∪ 그림자 캐스터 볼륨(CSM 캐스케이드, 로컬 그림자 장)"을
//   넘긴다 → 화면 밖이지만 화면 안으로 그림자를 드리우는 캐릭터도 팔레트가 나온다. 통과한 것만 팔레트와 정확한 경계를 계산한다.
//   통과하지 못한 엔티티는 IsCulled = true이며 그 프레임 어떤 패스에도 나오지 않는다 (정적 메시로 그려지면 안 된다).
// 호출: FScene::UpdateTransforms 이후, 그림자 캐스터 볼륨(캐스케이드/로컬 그림자 장)을 정한 뒤, 메시 인스턴스 Gather 전에 Build
class FSkinnedMeshPalette
{
public:
	using FVisibilityTest = std::function<bool(const FBox&)>;

	// IsVisible이 비어 있으면 모두 보이는 것으로
	void Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer, const FVisibilityTest& IsVisible = {});

	// 스킨으로 그릴 엔티티면 정보, 아니면 nullptr (FSkinComponent + 스킨 스트림 메시가 모두 있어야 함)
	const FSkinnedDrawInfo* Find(FEntity Entity) const;
	// 스킨 메시인데 이번 프레임 가시성 판정에서 빠진 엔티티 (그리지 않는다)
	bool IsCulled(FEntity Entity) const;

	// 프레임 팔레트 구조화 버퍼 (루트 SRV t15). Build한 프레임 안에서만 유효. 비어 있어도 유효한 주소
	D3D12_GPU_VIRTUAL_ADDRESS GetGpuData() const { return GpuData; }

	size_t GetCount() const { return Draws.size(); }
	uint32 GetCulledCount() const { return CulledCount; }
	uint32 GetBoneCount() const { return static_cast<uint32>(Bones.size()); }

	// 팔레트 계산 (테스트용 공개): Palette[i] = InverseBind[i] * JointWorld[i]
	static void ComputePalette(const std::vector<FMatrix4x4>& InverseBindMatrices, const std::vector<FMatrix4x4>& JointWorldMatrices,
	                           std::vector<FMatrix4x4>& OutPalette);
	// 조인트별 반경 (테스트용 공개): 바인드 경계 꼭짓점을 InverseBind[i]로 옮긴 점의 원점 거리 최댓값 (조인트 로컬 단위)
	static void ComputeJointRadii(const FBox& LocalBounds, const std::vector<FMatrix4x4>& InverseBindMatrices, std::vector<float>& OutRadii);
	// 보수적 월드 경계 (테스트용 공개): 조인트 위치들의 AABB ± max_i(Radii[i] × 조인트 3x3의 프로베니우스 노름 — 최대 특이값의 상한)
	static FBox ComputeConservativeBounds(const std::vector<FMatrix4x4>& JointWorldMatrices, const std::vector<float>& Radii);

private:
	// 엔티티 인덱스별 칸 (해시 대신 배열 — 세대로 검증)
	struct FEntitySlot
	{
		uint32 Generation = 0;
		uint64 Frame      = 0;     // 이 칸을 마지막으로 채운 Build 번호
		int32  Draw       = -1;    // Draws 번호 (-1 = 이번 프레임 컬링됨)
		// 조인트 반경 캐시
		const FStaticMesh* Mesh        = nullptr;
		const FMatrix4x4*  InverseBind = nullptr; // FSkinComponent::InverseBindMatrices.data() (바뀌면 다시 계산)
		size_t             Count       = 0;
		uint32             RadiiGeneration = ~0u;
		std::vector<float> Radii;
	};
	const FEntitySlot* FindSlot(FEntity Entity) const;

	std::vector<FSkinnedDrawInfo> Draws;
	std::vector<FEntitySlot>      Slots;
	uint32                        CulledCount = 0;
	std::vector<FMatrix4x4>                      Bones; // 이번 프레임 팔레트 (보이는 엔티티를 이어 붙임)
	std::vector<FMatrix4x4>                      JointWorldScratch;
	std::vector<FMatrix4x4>                      PaletteScratch;
	D3D12_GPU_VIRTUAL_ADDRESS                    GpuData    = 0;
	uint64                                       BuildCount = 0;
};

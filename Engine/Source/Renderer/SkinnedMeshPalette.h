#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "Scene/ResourceHandles.h"

#include <unordered_map>
#include <vector>

class FD3D12DynamicUploadBuffer;
class FResourceManager;
class FScene;
struct FEntity;

// 스킨 메시 엔티티 하나의 이번 프레임 드로우 정보
struct FSkinnedDrawInfo
{
	D3D12_GPU_VIRTUAL_ADDRESS Palette = 0; // SkinnedMesh.hlsli SkinBones (루트 CBV b4)
	FBox                      WorldBounds; // 조인트별 바인드 경계를 팔레트로 변환한 합집합 (보수적)
};

// 프레임마다 스킨 메시의 본 팔레트를 동적 업로드 버퍼에 한 번 올리고, 섀도우/메인 패스가 공유한다.
// 호출: FScene::UpdateTransforms 이후, 섀도우 패스 이전에 Build
class FSkinnedMeshPalette
{
public:
	void Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer);

	// 스킨으로 그릴 엔티티면 정보, 아니면 nullptr (FSkinComponent + 스킨 스트림 메시가 모두 있어야 함)
	const FSkinnedDrawInfo* Find(FEntity Entity) const;

	size_t GetCount() const { return Draws.size(); }

	// 팔레트 계산 (테스트용 공개): Palette[i] = InverseBind[i] * JointWorld[i]
	static void ComputePalette(const std::vector<FMatrix4x4>& InverseBindMatrices, const std::vector<FMatrix4x4>& JointWorldMatrices,
	                           std::vector<FMatrix4x4>& OutPalette);

private:
	std::unordered_map<uint64, FSkinnedDrawInfo> Draws; // 키: 엔티티 (인덱스 | 세대 << 32)
	std::vector<FMatrix4x4>                      JointWorldScratch;
	std::vector<FMatrix4x4>                      PaletteScratch;
};

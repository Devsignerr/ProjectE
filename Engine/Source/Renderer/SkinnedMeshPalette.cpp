#include "Renderer/SkinnedMeshPalette.h"

#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <cstring>

void FSkinnedMeshPalette::ComputePalette(const std::vector<FMatrix4x4>& InverseBindMatrices, const std::vector<FMatrix4x4>& JointWorldMatrices,
                                         std::vector<FMatrix4x4>& OutPalette)
{
	const size_t Count = FMath::Min(InverseBindMatrices.size(), JointWorldMatrices.size());
	OutPalette.resize(Count);
	for (size_t Index = 0; Index < Count; ++Index)
	{
		OutPalette[Index] = InverseBindMatrices[Index] * JointWorldMatrices[Index];
	}
}

void FSkinnedMeshPalette::Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer)
{
	Draws.clear();
	FRegistry& Registry = Scene.GetRegistry();

	Registry.View<FSkinComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FSkinComponent& Skin, FStaticMeshComponent& MeshComponent) {
		const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent.Mesh);
		if (Mesh == nullptr || !Mesh->IsSkinned() || Skin.Joints.empty())
		{
			return;
		}

		const size_t JointCount = FMath::Min<size_t>(Skin.Joints.size(), MaxSkinJoints);
		JointWorldScratch.resize(JointCount);
		for (size_t Index = 0; Index < JointCount; ++Index)
		{
			const FTransformComponent* Joint = Registry.IsValid(Skin.Joints[Index]) ? Registry.TryGet<FTransformComponent>(Skin.Joints[Index]) : nullptr;
			JointWorldScratch[Index]         = Joint ? Joint->WorldMatrix : FMatrix4x4::Identity;
		}
		ComputePalette(Skin.InverseBindMatrices, JointWorldScratch, PaletteScratch);

		const size_t                  Bytes      = FMath::Max<size_t>(PaletteScratch.size(), 1) * sizeof(FMatrix4x4);
		const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(Bytes);
		std::memcpy(Allocation.CpuAddress, PaletteScratch.data(), PaletteScratch.size() * sizeof(FMatrix4x4));

		// 정점은 바인드 공간 → 조인트별 팔레트로 월드. 가중 평균은 각 변환 결과의 볼록 결합이므로 합집합 경계에 포함된다
		FSkinnedDrawInfo Info;
		Info.Palette = Allocation.GpuAddress;
		for (const FMatrix4x4& Bone : PaletteScratch)
		{
			Info.WorldBounds.AddBox(Mesh->GetLocalBounds().TransformBy(Bone));
		}
		Draws.emplace(Entity.ToId(), Info);
	});
}

const FSkinnedDrawInfo* FSkinnedMeshPalette::Find(FEntity Entity) const
{
	const auto Found = Draws.find(Entity.ToId());
	return Found != Draws.end() ? &Found->second : nullptr;
}

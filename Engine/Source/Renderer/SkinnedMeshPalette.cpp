#include "Renderer/SkinnedMeshPalette.h"

#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <cstring>

namespace
{
	// 상자 변환 (중심 + 반 크기, 8꼭짓점 변환과 같은 결과의 AABB): 행벡터 규약 v * M
	FBox TransformBoxFast(const FVector3& Center, const FVector3& Extent, const FMatrix4x4& M)
	{
		FVector3 NewCenter(M.M[3][0], M.M[3][1], M.M[3][2]);
		FVector3 NewExtent(0.0f);
		for (int32 Column = 0; Column < 3; ++Column)
		{
			NewCenter[Column] += Center.X * M.M[0][Column] + Center.Y * M.M[1][Column] + Center.Z * M.M[2][Column];
			NewExtent[Column] = Extent.X * FMath::Abs(M.M[0][Column]) + Extent.Y * FMath::Abs(M.M[1][Column]) + Extent.Z * FMath::Abs(M.M[2][Column]);
		}
		return FBox(NewCenter - NewExtent, NewCenter + NewExtent);
	}
} // namespace

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

void FSkinnedMeshPalette::ComputeJointRadii(const FBox& LocalBounds, const std::vector<FMatrix4x4>& InverseBindMatrices, std::vector<float>& OutRadii)
{
	OutRadii.resize(InverseBindMatrices.size());
	for (size_t Joint = 0; Joint < InverseBindMatrices.size(); ++Joint)
	{
		// |v * InverseBind|는 v에 대해 볼록 → 상자 안 최댓값은 꼭짓점에서
		float MaxSquared = 0.0f;
		for (int32 Corner = 0; Corner < 8 && LocalBounds.IsValid(); ++Corner)
		{
			const FVector3 Point((Corner & 1) ? LocalBounds.Max.X : LocalBounds.Min.X, (Corner & 2) ? LocalBounds.Max.Y : LocalBounds.Min.Y,
			                     (Corner & 4) ? LocalBounds.Max.Z : LocalBounds.Min.Z);
			MaxSquared = FMath::Max(MaxSquared, InverseBindMatrices[Joint].TransformPosition(Point).LengthSquared());
		}
		OutRadii[Joint] = FMath::Sqrt(MaxSquared);
	}
}

FBox FSkinnedMeshPalette::ComputeConservativeBounds(const std::vector<FMatrix4x4>& JointWorldMatrices, const std::vector<float>& Radii)
{
	// 스킨 정점 = Σ w_i (u_i * JointWorld_i), u_i = v * InverseBind_i, |u_i| <= Radii[i].
	// |u * M3| <= |u| * ||M3||_F 이므로 각 항은 조인트 위치 중심 구 안 → 가중 평균(볼록 결합)은 구들의 AABB 합집합 안.
	// 빠르게: 조인트 위치 AABB를 가장 큰 반경(Radii[i]·||M3_i||_F의 최댓값)만큼 넓힌다 (더 보수적, 제곱근 한 번)
	FBox         Result;
	float        MaxReachSquared = 0.0f;
	const size_t Count           = FMath::Min(JointWorldMatrices.size(), Radii.size());
	for (size_t Joint = 0; Joint < Count; ++Joint)
	{
		const FMatrix4x4& M         = JointWorldMatrices[Joint];
		float             Frobenius = 0.0f;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Frobenius += M.M[Row][0] * M.M[Row][0] + M.M[Row][1] * M.M[Row][1] + M.M[Row][2] * M.M[Row][2];
		}
		MaxReachSquared = FMath::Max(MaxReachSquared, Radii[Joint] * Radii[Joint] * Frobenius);
		Result.AddPoint(FVector3(M.M[3][0], M.M[3][1], M.M[3][2]));
	}
	if (Result.IsValid())
	{
		const FVector3 Reach(FMath::Sqrt(MaxReachSquared));
		Result = FBox(Result.Min - Reach, Result.Max + Reach);
	}
	return Result;
}

void FSkinnedMeshPalette::Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer,
                                const FVisibilityTest& IsVisible)
{
	Draws.clear();
	Bones.clear();
	PrevBones.clear();
	CulledCount = 0;
	++BuildCount;
	FRegistry& Registry = Scene.GetRegistry();

	Registry.View<FSkinComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FSkinComponent& Skin, FStaticMeshComponent& MeshComponent) {
		const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent.Mesh);
		if (Mesh == nullptr || !Mesh->IsSkinned() || Skin.Joints.empty())
		{
			return;
		}

		if (Slots.size() <= Entity.Index)
		{
			Slots.resize(Entity.Index + 1);
		}
		FEntitySlot& Slot = Slots[Entity.Index];
		Slot.Generation   = Entity.Generation;
		Slot.Frame        = BuildCount;
		Slot.Draw         = -1;

		const size_t JointCount = FMath::Min<size_t>(Skin.Joints.size(), MaxSkinJoints);
		JointWorldScratch.resize(JointCount);
		for (size_t Index = 0; Index < JointCount; ++Index)
		{
			const FTransformComponent* Joint = Registry.IsValid(Skin.Joints[Index]) ? Registry.TryGet<FTransformComponent>(Skin.Joints[Index]) : nullptr;
			JointWorldScratch[Index]         = Joint ? Joint->WorldMatrix : FMatrix4x4::Identity;
		}

		// 가시성: 팔레트 전에 조인트 위치 기반 보수적 경계로 판정 (조인트 반경은 엔티티별 캐시)
		if (IsVisible && MeshComponent.bVisible)
		{
			if (Slot.RadiiGeneration != Entity.Generation || Slot.Mesh != Mesh || Slot.InverseBind != Skin.InverseBindMatrices.data() ||
			    Slot.Count != Skin.InverseBindMatrices.size())
			{
				ComputeJointRadii(Mesh->GetLocalBounds(), Skin.InverseBindMatrices, Slot.Radii);
				Slot.Mesh            = Mesh;
				Slot.InverseBind     = Skin.InverseBindMatrices.data();
				Slot.Count           = Skin.InverseBindMatrices.size();
				Slot.RadiiGeneration = Entity.Generation;
			}
			if (!IsVisible(ComputeConservativeBounds(JointWorldScratch, Slot.Radii)))
			{
				++CulledCount;
				return;
			}
		}

		ComputePalette(Skin.InverseBindMatrices, JointWorldScratch, PaletteScratch);

		// 정점은 바인드 공간 → 조인트별 팔레트로 월드. 가중 평균은 각 변환 결과의 볼록 결합이므로 합집합 경계에 포함된다
		FSkinnedDrawInfo Info;
		Info.BoneOffset              = static_cast<uint32>(Bones.size());
		Info.BoneCount               = static_cast<uint32>(PaletteScratch.size());
		const FBox&    LocalBounds   = Mesh->GetLocalBounds();
		const FVector3 LocalCenter   = LocalBounds.GetCenter();
		const FVector3 LocalExtent   = LocalBounds.GetExtent();
		for (const FMatrix4x4& Bone : PaletteScratch)
		{
			Info.WorldBounds.AddBox(TransformBoxFast(LocalCenter, LocalExtent, Bone));
		}
		Bones.insert(Bones.end(), PaletteScratch.begin(), PaletteScratch.end());

		// 이전 프레임 팔레트: 바로 앞 Build에서 같은 엔티티의 팔레트를 계산했을 때만 (PrevBones 안 위치, 업로드 때 Bones 뒤로 옮긴다)
		Info.PrevBoneOffset = ~0u;
		if (bTrackPrevious)
		{
			if (Slot.PaletteBuild + 1 == BuildCount && Slot.PaletteGeneration == Entity.Generation && Slot.PrevPalette.size() == PaletteScratch.size())
			{
				Info.PrevBoneOffset = static_cast<uint32>(PrevBones.size());
				PrevBones.insert(PrevBones.end(), Slot.PrevPalette.begin(), Slot.PrevPalette.end());
			}
			Slot.PrevPalette       = PaletteScratch;
			Slot.PaletteBuild      = BuildCount;
			Slot.PaletteGeneration = Entity.Generation;
		}
		Slot.Draw = static_cast<int32>(Draws.size());
		Draws.push_back(Info);
	});

	const uint32 CurrentCount = static_cast<uint32>(Bones.size());
	for (FSkinnedDrawInfo& Draw : Draws)
	{
		Draw.PrevBoneOffset = Draw.PrevBoneOffset == ~0u ? Draw.BoneOffset : Draw.PrevBoneOffset + CurrentCount;
	}

	// 팔레트 한 번에 업로드: [이번 프레임][이전 프레임] (빈 프레임도 루트 SRV가 유효한 주소를 가리키게 한 칸)
	const size_t                  Count      = FMath::Max<size_t>(Bones.size() + PrevBones.size(), 1);
	const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FMatrix4x4) * Count, 16);
	if (Bones.empty())
	{
		std::memcpy(Allocation.CpuAddress, &FMatrix4x4::Identity, sizeof(FMatrix4x4));
	}
	else
	{
		std::memcpy(Allocation.CpuAddress, Bones.data(), sizeof(FMatrix4x4) * Bones.size());
		if (!PrevBones.empty())
		{
			std::memcpy(static_cast<FMatrix4x4*>(Allocation.CpuAddress) + Bones.size(), PrevBones.data(), sizeof(FMatrix4x4) * PrevBones.size());
		}
	}
	GpuData = Allocation.GpuAddress;
}

const FSkinnedMeshPalette::FEntitySlot* FSkinnedMeshPalette::FindSlot(FEntity Entity) const
{
	if (Entity.Index >= Slots.size())
	{
		return nullptr;
	}
	const FEntitySlot& Slot = Slots[Entity.Index];
	return Slot.Frame == BuildCount && Slot.Generation == Entity.Generation ? &Slot : nullptr;
}

const FSkinnedDrawInfo* FSkinnedMeshPalette::Find(FEntity Entity) const
{
	const FEntitySlot* Slot = FindSlot(Entity);
	return Slot != nullptr && Slot->Draw >= 0 ? &Draws[Slot->Draw] : nullptr;
}

bool FSkinnedMeshPalette::IsCulled(FEntity Entity) const
{
	const FEntitySlot* Slot = FindSlot(Entity);
	return Slot != nullptr && Slot->Draw < 0;
}

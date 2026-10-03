#include "Renderer/SkinnedMeshPalette.h"

#include "Core/Jobs/ParallelFor.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
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

bool FSkinnedMeshPalette::IsSameSkin(const FCandidate& Candidate, const FGroup& Group)
{
	const FSkinComponent& Skin   = *Candidate.Skin;
	const FSkinComponent& Leader = *Candidates[Group.Leader].Skin;
	if (&Skin == &Leader)
	{
		return true;
	}
	if (Skin.Joints.size() != Leader.Joints.size() || Skin.InverseBindMatrices.size() != Leader.InverseBindMatrices.size() ||
	    !std::equal(Skin.Joints.begin(), Skin.Joints.end(), Leader.Joints.begin()))
	{
		return false;
	}
	// 역바인드 행렬 내용 비교는 (대표, 자기) 배열 주소가 지난번 확인 때와 같으면 생략 (조인트 반경 캐시와 같은 가정 — 배열을 바꾸면 주소가 바뀐다)
	FEntitySlot&      Slot        = Slots[Candidate.Entity.Index];
	const FMatrix4x4* LeaderData  = Leader.InverseBindMatrices.data();
	const FMatrix4x4* OwnData     = Skin.InverseBindMatrices.data();
	const size_t      MatrixCount = Skin.InverseBindMatrices.size();
	if (Slot.SharedLeaderInverseBind == LeaderData && Slot.SharedOwnInverseBind == OwnData && Slot.SharedCount == MatrixCount)
	{
		return true;
	}
	if (MatrixCount > 0 && std::memcmp(LeaderData, OwnData, sizeof(FMatrix4x4) * MatrixCount) != 0)
	{
		return false;
	}
	Slot.SharedLeaderInverseBind = LeaderData;
	Slot.SharedOwnInverseBind    = OwnData;
	Slot.SharedCount             = MatrixCount;
	return true;
}

void FSkinnedMeshPalette::Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer,
                                const FVisibilityTest& IsVisible)
{
	Draws.clear();
	Candidates.clear();
	Groups.clear();
	GroupBones.clear();
	CulledCount  = 0;
	PaletteCount = 0;
	BoneCount    = 0;
	++BuildCount;
	FRegistry& Registry = Scene.GetRegistry();

	// 1) 호출 스레드: 후보 수집 + 팔레트 공유 그룹 (그룹별 조인트 행렬 자리만 잡는다)
	Registry.View<FSkinComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FSkinComponent& Skin, FStaticMeshComponent& MeshComponent) {
		const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent.Mesh);
		if (Mesh == nullptr || !Mesh->IsReady() || !Mesh->IsSkinned() || Skin.Joints.empty())
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

		const uint32 CandidateIndex = static_cast<uint32>(Candidates.size());
		FCandidate&  Candidate      = Candidates.emplace_back();
		Candidate.Entity            = Entity;
		Candidate.Skin              = &Skin;
		Candidate.Mesh              = Mesh;
		Candidate.bTestVisibility   = IsVisible && MeshComponent.bVisible;

		// 첫 조인트가 같은 그룹들 중 같은 스킨을 찾는다
		const FEntity Root      = Skin.Joints[0];
		int32*        ChainHead = nullptr;
		if (Root.IsValid())
		{
			if (GroupByRootJoint.size() <= Root.Index)
			{
				GroupByRootJoint.resize(Root.Index + 1, { 0, -1 });
			}
			std::pair<uint64, int32>& Bucket = GroupByRootJoint[Root.Index];
			if (Bucket.first != BuildCount)
			{
				Bucket = { BuildCount, -1 };
			}
			ChainHead = &Bucket.second;
			for (int32 GroupIndex = Bucket.second; GroupIndex >= 0; GroupIndex = Groups[GroupIndex].NextSameRoot)
			{
				FGroup& Existing = Groups[GroupIndex];
				if (IsSameSkin(Candidate, Existing))
				{
					Candidate.Group                            = static_cast<uint32>(GroupIndex);
					Candidates[Existing.LastMember].NextMember = static_cast<int32>(CandidateIndex);
					Existing.LastMember                        = static_cast<int32>(CandidateIndex);
					return;
				}
			}
		}

		// 새 그룹
		const uint32 JointCount = static_cast<uint32>(FMath::Min<size_t>(Skin.Joints.size(), MaxSkinJoints));
		FGroup&      Group      = Groups.emplace_back();
		Group.Leader            = CandidateIndex;
		Group.LastMember        = static_cast<int32>(CandidateIndex);
		Group.JointOffset       = static_cast<uint32>(GroupBones.size());
		Group.JointCount        = JointCount;
		Group.PaletteCount      = static_cast<uint32>(FMath::Min<size_t>(Skin.InverseBindMatrices.size(), JointCount));
		Candidate.Group         = static_cast<uint32>(Groups.size() - 1);
		if (ChainHead != nullptr)
		{
			Group.NextSameRoot = *ChainHead;
			*ChainHead         = static_cast<int32>(Candidate.Group);
		}
		GroupBones.resize(GroupBones.size() + JointCount);
	});

	// 2) 병렬 (그룹마다, 쓰는 데이터는 그룹·구성원·그 엔티티 칸뿐): 조인트 월드 행렬 읽기(트랜스폼 풀 읽기 전용) →
	//    구성원 가시성(팔레트 전에 조인트 위치 기반 보수적 경계, 조인트 반경은 엔티티별 캐시) → 하나라도 보이면 제자리 팔레트 →
	//    보이는 구성원의 월드 경계 → 이전 프레임 팔레트 복사 + 대표 칸에 이번 팔레트 기록
	GroupPrevBones.resize(GroupBones.size());
	GroupJointScale.resize(GroupBones.size());
	// 해제된 조인트 엔티티는 컴포넌트가 없고 풀이 세대까지 비교하므로 풀 조회만으로 Registry.IsValid와 같은 결과
	const TSparseSet<FTransformComponent>* TransformPool = Registry.TryGetPool<FTransformComponent>();
	FParallel::ParallelFor(static_cast<uint32>(Groups.size()), 4, [&](uint32 Begin, uint32 End) {
		for (uint32 GroupIndex = Begin; GroupIndex < End; ++GroupIndex)
		{
			FGroup&     Group  = Groups[GroupIndex];
			FMatrix4x4* Bones  = GroupBones.data() + Group.JointOffset;
			Group.bVisible     = false;
			const std::vector<FEntity>& JointEntities = Candidates[Group.Leader].Skin->Joints;
			for (uint32 Index = 0; Index < Group.JointCount; ++Index)
			{
				const FTransformComponent* Joint = TransformPool != nullptr ? TransformPool->TryGet(JointEntities[Index]) : nullptr;
				Bones[Index]                     = Joint ? Joint->WorldMatrix : FMatrix4x4::Identity;
			}

			// 보수적 경계의 그룹 공통 부분 (ComputeConservativeBounds와 같은 식 — 구성원마다 조인트 반경만 다르다):
			// 조인트 위치 AABB와 조인트 3x3 프로베니우스 노름 제곱. 조인트 수 = min(조인트, 역바인드 수) = PaletteCount
			float* JointScale = GroupJointScale.data() + Group.JointOffset;
			FBox   JointBox;
			for (uint32 Joint = 0; Joint < Group.PaletteCount; ++Joint)
			{
				const FMatrix4x4& M         = Bones[Joint];
				float             Frobenius = 0.0f;
				for (int32 Row = 0; Row < 3; ++Row)
				{
					Frobenius += M.M[Row][0] * M.M[Row][0] + M.M[Row][1] * M.M[Row][1] + M.M[Row][2] * M.M[Row][2];
				}
				JointScale[Joint] = Frobenius;
				JointBox.AddPoint(FVector3(M.M[3][0], M.M[3][1], M.M[3][2]));
			}
			for (int32 Member = static_cast<int32>(Group.Leader); Member >= 0; Member = Candidates[Member].NextMember)
			{
				FCandidate& Candidate = Candidates[Member];
				if (!Candidate.bTestVisibility)
				{
					Candidate.bVisible = true;
					Group.bVisible     = true;
					continue;
				}
				const FSkinComponent& Skin = *Candidate.Skin;
				FEntitySlot&          Slot = Slots[Candidate.Entity.Index];
				if (Slot.RadiiGeneration != Candidate.Entity.Generation || Slot.Mesh != Candidate.Mesh ||
				    Slot.InverseBind != Skin.InverseBindMatrices.data() || Slot.Count != Skin.InverseBindMatrices.size())
				{
					ComputeJointRadii(Candidate.Mesh->GetLocalBounds(), Skin.InverseBindMatrices, Slot.Radii);
					Slot.Mesh            = Candidate.Mesh;
					Slot.InverseBind     = Skin.InverseBindMatrices.data();
					Slot.Count           = Skin.InverseBindMatrices.size();
					Slot.RadiiGeneration = Candidate.Entity.Generation;
				}
				FBox Conservative = JointBox;
				if (Conservative.IsValid())
				{
					float MaxReachSquared = 0.0f;
					for (uint32 Joint = 0; Joint < Group.PaletteCount; ++Joint)
					{
						MaxReachSquared = FMath::Max(MaxReachSquared, Slot.Radii[Joint] * Slot.Radii[Joint] * JointScale[Joint]);
					}
					const FVector3 Reach(FMath::Sqrt(MaxReachSquared));
					Conservative = FBox(Conservative.Min - Reach, Conservative.Max + Reach);
				}
				Candidate.bVisible = IsVisible(Conservative);
				Group.bVisible     = Group.bVisible || Candidate.bVisible;
			}
			if (!Group.bVisible)
			{
				continue;
			}

			// 팔레트[i] = InverseBind[i] * JointWorld[i] (제자리 — 이후 조인트 월드는 쓰지 않는다)
			const FCandidate& Leader      = Candidates[Group.Leader];
			const FMatrix4x4* InverseBind = Leader.Skin->InverseBindMatrices.data();
			for (uint32 Bone = 0; Bone < Group.PaletteCount; ++Bone)
			{
				Bones[Bone] = InverseBind[Bone] * Bones[Bone];
			}

			// 정점은 바인드 공간 → 조인트별 팔레트로 월드. 가중 평균은 각 변환 결과의 볼록 결합이므로 합집합 경계에 포함된다
			for (int32 Member = static_cast<int32>(Group.Leader); Member >= 0; Member = Candidates[Member].NextMember)
			{
				FCandidate& Candidate = Candidates[Member];
				Candidate.WorldBounds = FBox();
				if (!Candidate.bVisible)
				{
					continue;
				}
				const FBox&    LocalBounds = Candidate.Mesh->GetLocalBounds();
				const FVector3 LocalCenter = LocalBounds.GetCenter();
				const FVector3 LocalExtent = LocalBounds.GetExtent();
				for (uint32 Bone = 0; Bone < Group.PaletteCount; ++Bone)
				{
					Candidate.WorldBounds.AddBox(TransformBoxFast(LocalCenter, LocalExtent, Bones[Bone]));
				}
			}

			// 이전 프레임 팔레트: 대표가 바로 앞 Build에서도 대표로 팔레트를 남겼을 때만
			Group.bHasPrev = false;
			if (bTrackPrevious)
			{
				FEntitySlot& Slot = Slots[Leader.Entity.Index];
				if (Slot.PrevPaletteBuild + 1 == BuildCount && Slot.PaletteGeneration == Leader.Entity.Generation &&
				    Slot.PrevPalette.size() == Group.PaletteCount)
				{
					std::copy(Slot.PrevPalette.begin(), Slot.PrevPalette.end(), GroupPrevBones.begin() + Group.JointOffset);
					Group.bHasPrev = true;
				}
				Slot.PrevPalette.assign(Bones, Bones + Group.PaletteCount);
				Slot.PrevPaletteBuild = BuildCount;
			}
		}
	});

	// 3) 호출 스레드: 보이는 그룹의 업로드 자리 (그룹 순서), 드로우 정보 (후보 순서), 구성원 이력
	uint32 PrevBoneCount = 0;
	for (FGroup& Group : Groups)
	{
		if (!Group.bVisible)
		{
			continue;
		}
		++PaletteCount;
		Group.BoneOffset = BoneCount;
		BoneCount += Group.PaletteCount;
		Group.PrevOffset = ~0u;
		if (Group.bHasPrev)
		{
			Group.PrevOffset = PrevBoneCount;
			PrevBoneCount += Group.PaletteCount;
		}
	}
	for (const FCandidate& Candidate : Candidates)
	{
		if (!Candidate.bVisible)
		{
			++CulledCount;
			continue;
		}
		const FGroup&     Group = Groups[Candidate.Group];
		FEntitySlot&      Slot  = Slots[Candidate.Entity.Index];
		FSkinnedDrawInfo& Info  = Draws.emplace_back();
		Info.BoneOffset         = Group.BoneOffset;
		Info.BoneCount          = Group.PaletteCount;
		Info.PrevBoneOffset     = Group.BoneOffset;
		Info.WorldBounds        = Candidate.WorldBounds;
		if (bTrackPrevious)
		{
			// 구성원 이력: 바로 앞 Build에서도 같은 대표의 그룹에서 그려졌어야 한다
			const FEntity Leader = Candidates[Group.Leader].Entity;
			if (Group.PrevOffset != ~0u && Slot.PaletteBuild + 1 == BuildCount && Slot.PaletteGeneration == Candidate.Entity.Generation &&
			    Slot.PaletteLeader == Leader)
			{
				Info.PrevBoneOffset = BoneCount + Group.PrevOffset;
			}
			Slot.PaletteBuild      = BuildCount;
			Slot.PaletteGeneration = Candidate.Entity.Generation;
			Slot.PaletteLeader     = Leader;
		}
		Slot.Draw = static_cast<int32>(Draws.size() - 1);
	}

	// 팔레트 한 번에 업로드: [이번 프레임 (보이는 그룹 순)][이전 프레임 (이력 있는 그룹 순)] (빈 프레임도 루트 SRV가 유효한 주소를 가리키게 한 칸).
	// 업로드 힙(쓰기 결합)에는 순차로만 쓴다
	const size_t                  Count      = FMath::Max<size_t>(BoneCount + PrevBoneCount, 1);
	const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FMatrix4x4) * Count, 16);
	FMatrix4x4*                   Destination = static_cast<FMatrix4x4*>(Allocation.CpuAddress);
	if (BoneCount == 0)
	{
		std::memcpy(Destination, &FMatrix4x4::Identity, sizeof(FMatrix4x4));
	}
	for (const FGroup& Group : Groups)
	{
		if (Group.bVisible)
		{
			std::memcpy(Destination + Group.BoneOffset, GroupBones.data() + Group.JointOffset, sizeof(FMatrix4x4) * Group.PaletteCount);
		}
	}
	for (const FGroup& Group : Groups)
	{
		if (Group.bVisible && Group.PrevOffset != ~0u)
		{
			std::memcpy(Destination + BoneCount + Group.PrevOffset, GroupPrevBones.data() + Group.JointOffset, sizeof(FMatrix4x4) * Group.PaletteCount);
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

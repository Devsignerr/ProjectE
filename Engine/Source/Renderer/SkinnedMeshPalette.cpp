#include "Renderer/SkinnedMeshPalette.h"

#include "Core/Jobs/ParallelFor.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <emmintrin.h>
#include <cstring>

namespace
{
	// 그룹 묶기 1차 비교 키: 조인트 엔티티 목록 + 역바인드 행렬 수 (FNV-1a). 같은 스킨이면 반드시 같다
	uint64 HashJoints(const FSkinComponent& Skin)
	{
		uint64 Hash = 14695981039346656037ull;
		auto   Mix  = [&Hash](uint64 Value) {
			Hash ^= Value;
			Hash *= 1099511628211ull;
		};
		Mix(Skin.Joints.size());
		Mix(Skin.InverseBindMatrices.size());
		for (const FEntity Joint : Skin.Joints)
		{
			Mix((static_cast<uint64>(Joint.Generation) << 32) | Joint.Index);
		}
		return Hash;
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

FBox FSkinnedMeshPalette::ComputeSkinnedBounds(const FBox& LocalBounds, const FMatrix4x4* Palette, uint32 Count)
{
	// 스칼라 식 (TransformBox: 중심 = M3 + ((Cx*M0 + Cy*M1) + Cz*M2), 반 크기 = (Ex*Abs(M0) + Ey*Abs(M1)) + Ez*Abs(M2), 열마다) 을
	// 열 x, y, z를 SSE 레인으로 같은 순서로 계산한다. Abs = (v < 0 ? -v : v) (FMath::Abs — -0 유지), 최소/최대 = MINPS/MAXPS
	// (FMath::Min(A, B) = A < B ? A : B와 같은 선택), 유효하지 않은 상자(NaN)는 AddBox처럼 건너뛴다 → 비트 단위로 같은 결과
	const FVector3 Center = LocalBounds.GetCenter();
	const FVector3 Extent = LocalBounds.GetExtent();
	const __m128   Cx     = _mm_set1_ps(Center.X);
	const __m128   Cy     = _mm_set1_ps(Center.Y);
	const __m128   Cz     = _mm_set1_ps(Center.Z);
	const __m128   Ex     = _mm_set1_ps(Extent.X);
	const __m128   Ey     = _mm_set1_ps(Extent.Y);
	const __m128   Ez     = _mm_set1_ps(Extent.Z);
	const __m128   Zero   = _mm_setzero_ps();
	const __m128   Sign   = _mm_set1_ps(-0.0f);
	auto           Abs    = [&](__m128 Value) {
		const __m128 Negative = _mm_cmplt_ps(Value, Zero);
		return _mm_or_ps(_mm_and_ps(Negative, _mm_xor_ps(Value, Sign)), _mm_andnot_ps(Negative, Value));
	};
	const FBox Empty;
	__m128     Min = _mm_setr_ps(Empty.Min.X, Empty.Min.Y, Empty.Min.Z, 0.0f);
	__m128     Max = _mm_setr_ps(Empty.Max.X, Empty.Max.Y, Empty.Max.Z, 0.0f);
	for (uint32 Bone = 0; Bone < Count; ++Bone)
	{
		const FMatrix4x4& M      = Palette[Bone];
		const __m128      Row0   = _mm_loadu_ps(M.M[0]);
		const __m128      Row1   = _mm_loadu_ps(M.M[1]);
		const __m128      Row2   = _mm_loadu_ps(M.M[2]);
		const __m128      Row3   = _mm_loadu_ps(M.M[3]);
		const __m128      Middle = _mm_add_ps(Row3, _mm_add_ps(_mm_add_ps(_mm_mul_ps(Cx, Row0), _mm_mul_ps(Cy, Row1)), _mm_mul_ps(Cz, Row2)));
		const __m128      Half   = _mm_add_ps(_mm_add_ps(_mm_mul_ps(Ex, Abs(Row0)), _mm_mul_ps(Ey, Abs(Row1))), _mm_mul_ps(Ez, Abs(Row2)));
		const __m128      BoxMin = _mm_sub_ps(Middle, Half);
		const __m128      BoxMax = _mm_add_ps(Middle, Half);
		if ((_mm_movemask_ps(_mm_cmple_ps(BoxMin, BoxMax)) & 7) != 7)
		{
			continue; // FBox::AddBox: 유효하지 않은 상자는 더하지 않는다
		}
		Min = _mm_min_ps(_mm_min_ps(Min, BoxMin), BoxMax); // AddPoint(Min) → AddPoint(Max) 순서
		Max = _mm_max_ps(_mm_max_ps(Max, BoxMin), BoxMax);
	}
	alignas(16) float MinValues[4];
	alignas(16) float MaxValues[4];
	_mm_store_ps(MinValues, Min);
	_mm_store_ps(MaxValues, Max);
	return FBox(FVector3(MinValues[0], MinValues[1], MinValues[2]), FVector3(MaxValues[0], MaxValues[1], MaxValues[2]));
}

void FSkinnedMeshPalette::GroupCandidates(const FVisibilityTest& IsVisible, bool bExact)
{
	Candidates.clear();
	Groups.clear();
	GroupJointTotal = 0;
	++GroupPassCount;
	for (const FLookup& Lookup : Lookups)
	{
		if (Lookup.Mesh == nullptr)
		{
			continue;
		}
		const uint32 CandidateIndex = static_cast<uint32>(Candidates.size());
		FCandidate&  Candidate      = Candidates.emplace_back();
		Candidate.Entity            = Lookup.Entity;
		Candidate.Skin              = Lookup.Skin;
		Candidate.Mesh              = Lookup.Mesh;
		Candidate.JointKey          = Lookup.JointKey;
		Candidate.JointsSize        = Lookup.JointsSize;
		Candidate.InverseBindSize   = Lookup.InverseBindSize;
		Candidate.bTestVisibility   = IsVisible && Lookup.bMeshVisible;

		// 첫 조인트가 같은 그룹들 중 같은 스킨을 찾는다 (사슬 = 최근에 만든 그룹부터)
		const FEntity Root      = Lookup.RootJoint;
		int32*        ChainHead = nullptr;
		bool          bJoined   = false;
		if (Root.IsValid())
		{
			if (GroupByRootJoint.size() <= Root.Index)
			{
				GroupByRootJoint.resize(Root.Index + 1, { 0, -1 });
			}
			std::pair<uint64, int32>& Bucket = GroupByRootJoint[Root.Index];
			if (Bucket.first != GroupPassCount)
			{
				Bucket = { GroupPassCount, -1 };
			}
			ChainHead = &Bucket.second;
			for (int32 GroupIndex = Bucket.second; GroupIndex >= 0 && !bJoined; GroupIndex = Groups[GroupIndex].NextSameRoot)
			{
				FGroup& Existing = Groups[GroupIndex];
				if (bExact)
				{
					bJoined = IsSameSkin(Candidate, Existing);
				}
				else
				{
					// 키가 같으면 일단 묶고 EvaluateGroups가 병렬로 확인한다. IsSameSkin이 참이면 키도 같으므로 확인이 모두 통과하면
					// 사슬에서 처음 맞는 그룹 = 정확 비교로 처음 맞는 그룹
					const FCandidate& Leader = Candidates[Existing.Leader];
					bJoined = Leader.JointKey == Candidate.JointKey && Leader.JointsSize == Candidate.JointsSize &&
					          Leader.InverseBindSize == Candidate.InverseBindSize;
					Candidate.bVerify = bJoined && Leader.Skin != Candidate.Skin;
				}
				if (bJoined)
				{
					Candidate.Group                            = static_cast<uint32>(GroupIndex);
					Candidates[Existing.LastMember].NextMember = static_cast<int32>(CandidateIndex);
					Existing.LastMember                        = static_cast<int32>(CandidateIndex);
				}
			}
		}
		if (bJoined)
		{
			continue;
		}

		// 새 그룹
		const uint32 JointCount = static_cast<uint32>(FMath::Min<size_t>(Lookup.JointsSize, MaxSkinJoints));
		FGroup&      Group      = Groups.emplace_back();
		Group.Leader            = CandidateIndex;
		Group.LastMember        = static_cast<int32>(CandidateIndex);
		Group.JointOffset       = GroupJointTotal;
		Group.JointCount        = JointCount;
		Group.PaletteCount      = static_cast<uint32>(FMath::Min<size_t>(Lookup.InverseBindSize, JointCount));
		Candidate.Group         = static_cast<uint32>(Groups.size() - 1);
		if (ChainHead != nullptr)
		{
			Group.NextSameRoot = *ChainHead;
			*ChainHead         = static_cast<int32>(Candidate.Group);
		}
		GroupJointTotal += JointCount;
	}
	// 조인트 행렬 칸은 줄이지 않는다 (EvaluateGroups가 그룹마다 JointCount만큼 모두 덮어쓴다 — 매 프레임 초기화 생략)
	if (GroupBones.size() < GroupJointTotal)
	{
		GroupBones.resize(GroupJointTotal);
	}
	if (GroupJointScale.size() < GroupJointTotal)
	{
		GroupJointScale.resize(GroupJointTotal);
	}
}

bool FSkinnedMeshPalette::EvaluateGroups(const FScene& Scene, const FVisibilityTest& IsVisible)
{
	// 병렬 (그룹마다, 쓰는 데이터는 그룹·구성원·그 엔티티 칸뿐): 키로 묶은 구성원 확인 → 조인트 월드 행렬 읽기(트랜스폼 풀 읽기 전용) →
	// 구성원 가시성(팔레트 전에 조인트 위치 기반 보수적 경계, 조인트 반경은 엔티티별 캐시) → 이력 판정
	// 해제된 조인트 엔티티는 컴포넌트가 없고 풀이 세대까지 비교하므로 풀 조회만으로 Registry.IsValid와 같은 결과
	const TSparseSet<FTransformComponent>* TransformPool = Scene.GetRegistry().TryGetPool<FTransformComponent>();
	std::atomic<bool>                      bMismatch     = false;
	FParallel::ParallelFor(static_cast<uint32>(Groups.size()), 4, [&](uint32 Begin, uint32 End) {
		for (uint32 GroupIndex = Begin; GroupIndex < End; ++GroupIndex)
		{
			FGroup&     Group  = Groups[GroupIndex];
			FMatrix4x4* Bones  = GroupBones.data() + Group.JointOffset;
			Group.bVisible     = false;
			Group.bHasPrev     = false;
			for (int32 Member = Candidates[Group.Leader].NextMember; Member >= 0; Member = Candidates[Member].NextMember)
			{
				const FCandidate& Candidate = Candidates[Member];
				if (Candidate.bVerify && !IsSameSkin(Candidate, Group)) // 쓰는 것은 자기 엔티티 칸의 비교 캐시뿐
				{
					bMismatch = true;
				}
			}

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

			// 이전 프레임 팔레트: 대표가 바로 앞 Build에서도 대표로 팔레트를 남겼을 때만
			if (Group.bVisible && bTrackPrevious)
			{
				const FCandidate&  Leader = Candidates[Group.Leader];
				const FEntitySlot& Slot   = Slots[Leader.Entity.Index];
				Group.bHasPrev = Slot.PrevPaletteBuild + 1 == BuildCount && Slot.PaletteGeneration == Leader.Entity.Generation &&
				                 Slot.PrevPaletteCount == Group.PaletteCount;
			}
		}
	});
	return !bMismatch;
}

void FSkinnedMeshPalette::Build(FScene& Scene, const FResourceManager& Resources, FD3D12DynamicUploadBuffer& DynamicBuffer,
                                const FVisibilityTest& IsVisible)
{
	CulledCount  = 0;
	PaletteCount = 0;
	BoneCount    = 0;
	++BuildCount;
	GroupBones.swap(PrevGroupBones); // 바로 앞 Build의 팔레트(보인 그룹은 제자리 팔레트)가 PrevGroupBones에 남는다
	FRegistry& Registry = Scene.GetRegistry();

	// 1) 병렬: View<FSkinComponent, FStaticMeshComponent>와 같은 순서·조건으로 칸마다 메시 해석 + 조인트 키
	const std::vector<FEntity>* ViewEntities = Registry.View<FSkinComponent, FStaticMeshComponent>().GetIterationEntities();
	Lookups.resize(ViewEntities != nullptr ? ViewEntities->size() : 0);
	if (!Lookups.empty())
	{
		const TSparseSet<FSkinComponent>*       SkinPool = Registry.TryGetPool<FSkinComponent>();
		const TSparseSet<FStaticMeshComponent>* MeshPool = Registry.TryGetPool<FStaticMeshComponent>();
		FParallel::ParallelFor(static_cast<uint32>(Lookups.size()), 128, [&](uint32 Begin, uint32 End) {
			for (uint32 Index = Begin; Index < End; ++Index)
			{
				FLookup& Lookup = Lookups[Index];
				Lookup          = FLookup{};
				Lookup.Entity   = (*ViewEntities)[Index];
				const FSkinComponent*       Skin          = SkinPool->TryGet(Lookup.Entity);
				const FStaticMeshComponent* MeshComponent = MeshPool->TryGet(Lookup.Entity);
				if (Skin == nullptr || MeshComponent == nullptr)
				{
					continue;
				}
				const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent->Mesh);
				if (Mesh == nullptr || !Mesh->IsReady() || !Mesh->IsSkinned() || Skin->Joints.empty())
				{
					continue;
				}
				Lookup.Skin            = Skin;
				Lookup.Mesh            = Mesh;
				Lookup.RootJoint       = Skin->Joints[0];
				Lookup.JointsSize      = static_cast<uint32>(Skin->Joints.size());
				Lookup.InverseBindSize = static_cast<uint32>(Skin->InverseBindMatrices.size());
				Lookup.bMeshVisible    = MeshComponent->bVisible;
				Lookup.JointKey        = HashJoints(*Skin);
			}
		});
	}
	// 엔티티 칸 크기 (병렬 단계는 크기를 바꾸지 않는다)
	uint32 MaxEntityIndex = 0;
	for (const FLookup& Lookup : Lookups)
	{
		MaxEntityIndex = Lookup.Mesh != nullptr ? std::max(MaxEntityIndex, Lookup.Entity.Index + 1) : MaxEntityIndex;
	}
	if (Slots.size() < MaxEntityIndex)
	{
		Slots.resize(MaxEntityIndex);
		EntityDraws.resize(MaxEntityIndex);
	}

	// 2) 호출 스레드: 팔레트 공유 그룹 (조인트 키로 묶기) → 병렬: 그룹 평가 (키로 묶은 구성원 확인 포함).
	//    키 충돌(확인 실패)이면 정확 비교로 다시 묶고 다시 평가한다
	GroupCandidates(IsVisible, false);
	if (!EvaluateGroups(Scene, IsVisible))
	{
		GroupCandidates(IsVisible, true);
		EvaluateGroups(Scene, IsVisible);
	}

	// 3) 호출 스레드: 보이는 그룹의 업로드 자리 (그룹 순서), 드로우 번호 (후보 순서).
	//    버퍼 = [이번 프레임 (보이는 그룹 순)][이전 프레임 (이력 있는 그룹 순)]
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
	uint32 DrawCount = 0;
	for (FCandidate& Candidate : Candidates)
	{
		Candidate.Draw = Candidate.bVisible ? static_cast<int32>(DrawCount++) : -1;
	}
	CulledCount = static_cast<uint32>(Candidates.size()) - DrawCount;
	Draws.resize(DrawCount);
	// 빈 프레임도 루트 SRV가 유효한 주소를 가리키게 한 칸
	const size_t                  UploadCount = FMath::Max<size_t>(BoneCount + PrevBoneCount, 1);
	const FD3D12DynamicAllocation Allocation  = DynamicBuffer.Allocate(sizeof(FMatrix4x4) * UploadCount, 16);
	FMatrix4x4*                   Destination = static_cast<FMatrix4x4*>(Allocation.CpuAddress);
	if (BoneCount == 0)
	{
		std::memcpy(Destination, &FMatrix4x4::Identity, sizeof(FMatrix4x4));
	}

	// 4) 병렬 (그룹마다, 쓰는 데이터는 그룹·구성원·그 드로우·엔티티 칸뿐): 제자리 팔레트 → 보이는 구성원의 월드 경계 →
	//    업로드 버퍼에 이번/이전 팔레트를 구간마다 연속으로 쓴다 (업로드 힙은 쓰기 결합 — 읽지 않는다) → 대표 칸에 이번 팔레트 기록 →
	//    구성원 드로우 정보·이력·이번 Build 결과
	const uint32 CurrentBoneCount = BoneCount;
	const uint32 Build32          = static_cast<uint32>(BuildCount);
	FParallel::ParallelFor(static_cast<uint32>(Groups.size()), 4, [&](uint32 Begin, uint32 End) {
		for (uint32 GroupIndex = Begin; GroupIndex < End; ++GroupIndex)
		{
			const FGroup&     Group  = Groups[GroupIndex];
			const FCandidate& Leader = Candidates[Group.Leader];
			if (Group.bVisible)
			{
				FMatrix4x4* Bones = GroupBones.data() + Group.JointOffset;

				// 팔레트[i] = InverseBind[i] * JointWorld[i] (제자리 — 이후 조인트 월드는 쓰지 않는다)
				const FMatrix4x4* InverseBind = Leader.Skin->InverseBindMatrices.data();
				for (uint32 Bone = 0; Bone < Group.PaletteCount; ++Bone)
				{
					Bones[Bone] = InverseBind[Bone] * Bones[Bone];
				}

				// 정점은 바인드 공간 → 조인트별 팔레트로 월드. 가중 평균은 각 변환 결과의 볼록 결합이므로 합집합 경계에 포함된다
				for (int32 Member = static_cast<int32>(Group.Leader); Member >= 0; Member = Candidates[Member].NextMember)
				{
					FCandidate& Candidate = Candidates[Member];
					Candidate.WorldBounds = Candidate.bVisible ? ComputeSkinnedBounds(Candidate.Mesh->GetLocalBounds(), Bones, Group.PaletteCount) : FBox();
				}

				std::memcpy(Destination + Group.BoneOffset, Bones, sizeof(FMatrix4x4) * Group.PaletteCount);
				if (bTrackPrevious)
				{
					FEntitySlot& Slot = Slots[Leader.Entity.Index];
					if (Group.PrevOffset != ~0u)
					{
						std::memcpy(Destination + CurrentBoneCount + Group.PrevOffset, PrevGroupBones.data() + Slot.PrevPaletteOffset,
						            sizeof(FMatrix4x4) * Group.PaletteCount);
					}
					Slot.PrevPaletteOffset = Group.JointOffset; // 다음 Build에는 PrevGroupBones가 된다
					Slot.PrevPaletteCount  = Group.PaletteCount;
					Slot.PrevPaletteBuild  = BuildCount;
				}
			}

			for (int32 Member = static_cast<int32>(Group.Leader); Member >= 0; Member = Candidates[Member].NextMember)
			{
				const FCandidate& Candidate = Candidates[Member];
				EntityDraws[Candidate.Entity.Index] = { Candidate.Entity.Generation, Build32, Candidate.Draw };
				if (Candidate.Draw < 0)
				{
					continue;
				}
				FEntitySlot&      Slot = Slots[Candidate.Entity.Index];
				FSkinnedDrawInfo& Info = Draws[Candidate.Draw];
				Info.BoneOffset        = Group.BoneOffset;
				Info.BoneCount         = Group.PaletteCount;
				Info.PrevBoneOffset    = Group.BoneOffset;
				Info.WorldBounds       = Candidate.WorldBounds;
				if (bTrackPrevious)
				{
					// 구성원 이력: 바로 앞 Build에서도 같은 대표의 그룹에서 그려졌어야 한다
					if (Group.PrevOffset != ~0u && Slot.PaletteBuild + 1 == BuildCount && Slot.PaletteGeneration == Candidate.Entity.Generation &&
					    Slot.PaletteLeader == Leader.Entity)
					{
						Info.PrevBoneOffset = CurrentBoneCount + Group.PrevOffset;
					}
					Slot.PaletteBuild      = BuildCount;
					Slot.PaletteGeneration = Candidate.Entity.Generation;
					Slot.PaletteLeader     = Leader.Entity;
				}
			}
		}
	});
	GpuData = Allocation.GpuAddress;
}

const FSkinnedMeshPalette::FEntityDraw* FSkinnedMeshPalette::FindDraw(FEntity Entity) const
{
	if (Entity.Index >= EntityDraws.size())
	{
		return nullptr;
	}
	const FEntityDraw& Draw = EntityDraws[Entity.Index];
	return Draw.Build == static_cast<uint32>(BuildCount) && Draw.Generation == Entity.Generation ? &Draw : nullptr;
}

const FSkinnedDrawInfo* FSkinnedMeshPalette::Find(FEntity Entity) const
{
	const FEntityDraw* Draw = FindDraw(Entity);
	return Draw != nullptr && Draw->Draw >= 0 ? &Draws[Draw->Draw] : nullptr;
}

bool FSkinnedMeshPalette::IsCulled(FEntity Entity) const
{
	const FEntityDraw* Draw = FindDraw(Entity);
	return Draw != nullptr && Draw->Draw < 0;
}

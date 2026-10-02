#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct FModelMetadata;

// 애니메이션 리타기팅 (언리얼 IK Retargeter의 FK 체인 부분을 단순화): 한 스켈레톤의 클립을 다른 스켈레톤 모델에서 재생한다.
//
// 리그: 휴머노이드 표준 뼈 23개 (EHumanoidBone — Root/Hips/Spine/Chest/UpperChest/Neck/Head + 좌우 Shoulder·UpperArm·LowerArm·Hand
//   + 좌우 UpperLeg·LowerLeg·Foot·Toes). 손가락 등 리그 밖 뼈는 매핑하지 않는다 (대상 기본 포즈 유지).
// 매핑 (모델마다): 자동 추정 + 모델 사이드카 .emeta "Retarget" 수동 지정(리그 뼈 → 노드 이름, 빈 이름 = 매핑 안 함)이 덮는다.
//   자동 추정 (AnimRetargetMath::AutoMap — 이름 규칙 + 계층):
//     이름 정리 = 소문자, ':'/'|' 앞 네임스페이스와 "mixamorig" 제거 → 좌우 토큰 분리 ("left"/"right" 포함, 끝 ".l" "_l" "-l" " l",
//     앞 "l_" "l." "l-" "l ") → 영문자만 남긴 기본 이름. 기본 이름 → 리그 뼈 (Mixamo/UE/Unity/KayKit/Quaternius/Blender 관례):
//       Root=root, Hips=hips|pelvis|hip, Neck=neck, Head=head, Shoulder=shoulder|clavicle|collar,
//       UpperArm=upperarm|arm, LowerArm=lowerarm|forearm, Hand=wrist(우선)|hand,
//       UpperLeg=upperleg|thigh|upleg, LowerLeg=lowerleg|calf|leg|shin, Foot=foot|ankle, Toes=toes|toe|toebase|ball.
//     Spine/Chest/UpperChest는 이름이 아니라 계층: Hips와 Neck(없으면 Head) 사이 조상 사슬을 위에서부터 Spine, Chest, UpperChest
//       (4개 이상이면 Spine=첫째, UpperChest=마지막, Chest=가운데).
//     좌우 토큰이 없으면 기본 포즈 모델 공간 Y로 가른다 (+Y = 왼쪽 — glTF 캐릭터가 +Z(엔진 -X)를 보는 관례).
//     같은 리그 뼈 후보가 여럿이면 이름 우선순위 → 얕은 노드. 사슬 검증: LowerArm은 UpperArm 자손, Hand는 LowerArm 자손,
//     LowerLeg는 UpperLeg 자손, Foot은 LowerLeg 자손, Toes는 Foot 자손, 팔/다리/목/머리는 Hips 자손이어야 한다 (아니면 버림).
//     Root는 Hips의 조상일 때만.
// 변환 (AnimRetargetMath::RetargetClip — 소스와 대상 모두 매핑된 리그 뼈만, 30Hz로 다시 샘플링):
//   회전 = 모델 공간 바인드 보정: Gd(t) = F · Gs(t) · Gs(bind)⁻¹ · F⁻¹ · Align · Gd(bind)
//     Gs/Gd = 소스/대상 뼈의 모델 공간 회전, bind = 기본(임포트) 포즈, F = 정면 보정(Z축 회전 — 왼다리→오른다리 수평 방향을 맞춤,
//     다리가 없으면 항등), Align = 대상 기본 포즈의 뼈 방향(그 뼈 → 리그 자식 뼈)을 소스 기본 포즈 방향(F 적용)으로 돌리는 최소 회전
//     (A 포즈 ↔ T 포즈 차이 보정. 방향 자식이 양쪽에 없으면(손/머리/발끝) 리그 부모의 Align을 이어받고, Root/Hips는 항등).
//     로컬 회전 = (대상 부모의 그 시각 모델 회전)⁻¹ · Gd(t) — 부모부터 차례로 계산한다.
//   이동 = Root/Hips만: 대상 모델 위치 = Pd(bind) + F · (Ps(t) - Ps(bind)) × 키 비율, 키 비율 = 대상 Hips 높이 / 소스 Hips 높이
//     (기본 포즈 모델 공간 Z, 1cm 이하면 1). 로컬 이동 = 그 시각 대상 부모 모델 행렬의 역으로. 나머지 뼈 이동은 대상 기본 포즈 유지.
//   루트 모션은 Root(없으면 Hips) 이동 채널에 실려 와 키 비율로 스케일된다 (Scene/AnimRootMotion.h).
// 사용 (FAnimationSystem): FAnimationComponent::RetargetSources(Content 기준 모델 경로, ';'/',' 구분) 또는 클립 이름 "<모델 경로>:<클립>"
//   (컴포넌트 Clip, 그래프 상태 샘플, Play/PlayMontage)이 다른 모델을 가리키면 그 모델의 클립 전부를 리타기팅해 이 모델의 클립 뒤에 덧붙인
//   세트를 만든다 (FAnimationRuntime::BaseSet → Set). 덧붙인 클립은 원래 이름(이미 같은 이름이 있으면 모델 자신/앞 소스가 이김)과
//   "<모델 경로>:<클립>" 별칭으로 찾는다 → 그래프/블렌드 스페이스/몽타주가 그대로 동작한다. 노티파이는 소스 모델 .emeta 기준
//   (FAnimationSet::ClipNotifySources).
// 캐시: FAnimRetargetLibrary — 소스 모델(경로별 한 번 로드) + 리타기팅 결과(소스 경로 × 대상 키(모델 경로)). 매핑(.emeta)을 고치면
//   Invalidate(모델 경로) → 세대 증가 → 사용 중인 컴포넌트가 다음 갱신에서 다시 만든다.
// 한계: 모델 공간 정면이 같다고 보고 F는 Z축 회전만 보정한다(누운 모델 불가). 비균등 스케일 뼈, IK 리그(발이 다리 사슬 밖)는 맞지 않는다.

enum class EHumanoidBone : uint8
{
	Root,
	Hips,
	Spine,
	Chest,
	UpperChest,
	Neck,
	Head,
	LeftShoulder,
	LeftUpperArm,
	LeftLowerArm,
	LeftHand,
	RightShoulder,
	RightUpperArm,
	RightLowerArm,
	RightHand,
	LeftUpperLeg,
	LeftLowerLeg,
	LeftFoot,
	LeftToes,
	RightUpperLeg,
	RightLowerLeg,
	RightFoot,
	RightToes,
	Count,
};

inline constexpr size_t HumanoidBoneCount = static_cast<size_t>(EHumanoidBone::Count);

// 리그 뼈 → 모델 노드 번호 (-1 = 없음)
using FHumanoidMapping = std::array<int32, HumanoidBoneCount>;

enum class EBoneSide : uint8
{
	None,
	Left,
	Right,
};

// 이름 규칙 결과 (테스트 대상)
struct FBoneNameInfo
{
	EBoneSide   Side = EBoneSide::None;
	std::string Base; // 소문자 영문자만
};

// 소스 또는 대상 스켈레톤 (기본 포즈 모델 공간 정보 포함)
struct FRetargetSkeleton
{
	std::vector<int32>      Parents;
	std::vector<FNodePose>  RestPose;
	std::vector<FMatrix4x4> RestMatrices;  // 모델 공간
	std::vector<FQuat>      RestRotations; // 모델 공간 회전
	FHumanoidMapping        Mapping{};
};

// 소스 → 대상 변환 준비물 (스켈레톤 쌍마다 한 번)
struct FRetargetPlan
{
	const FRetargetSkeleton*                Source = nullptr;
	const FRetargetSkeleton*                Target = nullptr;
	std::array<FQuat, HumanoidBoneCount>    Align{};      // 대상 기본 포즈 → 기준 포즈 보정 (모델 공간)
	std::array<uint8, HumanoidBoneCount>    bActive{};    // 양쪽 모두 매핑됨
	std::vector<EHumanoidBone>              Order;        // 대상 노드 깊이 순 (부모 먼저)
	FQuat                                   Facing;       // 소스 모델 공간 → 대상 모델 공간 Z축 회전
	float                                   HeightRatio = 1.0f;
};

namespace AnimRetargetMath
{
	const char*   GetBoneName(EHumanoidBone Bone); // "LeftUpperArm" 등 (.emeta 키)
	EHumanoidBone FindBone(std::string_view Name);  // 없으면 Count
	// 정렬 보정을 이어받을 리그 부모 (Root/Hips는 Count)
	EHumanoidBone GetParentBone(EHumanoidBone Bone);
	// 방향을 정하는 리그 자식 후보 (가까운 순 — 양쪽에 있는 첫 뼈를 쓴다)
	std::vector<EHumanoidBone> GetDirectionChildren(EHumanoidBone Bone);

	FBoneNameInfo ParseBoneName(std::string_view Name);

	// 이름 규칙 + 계층으로 매핑 추정 (머리 주석). RestMatrices = 기본 포즈 모델 공간 행렬 (좌우 판정용, 비면 이름만)
	FHumanoidMapping AutoMap(const std::vector<std::string>& Names, const std::vector<int32>& Parents, const std::vector<FMatrix4x4>& RestMatrices);
	// 자동 추정 + .emeta 수동 지정 (Metadata가 nullptr이면 자동 그대로). 이름을 못 찾은 수동 지정은 그 뼈를 매핑하지 않는다
	FHumanoidMapping ResolveMapping(const FAnimationSet& Set, const FModelMetadata* Metadata);

	// Gd = Source · SourceBind⁻¹ · TargetReference (모두 모델 공간, Facing 적용 전/후는 호출하는 쪽이 맞춘다)
	FQuat RetargetRotation(const FQuat& SourceBind, const FQuat& Source, const FQuat& TargetReference);
	// From 방향을 To 방향으로 돌리는 최소 회전 (둘 중 하나가 0이면 항등)
	FQuat MakeRotationBetween(const FVector3& From, const FVector3& To);
	// 키 비율 = 대상 Hips 높이 / 소스 Hips 높이 (둘 중 하나가 1cm 이하면 1)
	float ComputeHeightRatio(float SourceHipsHeight, float TargetHipsHeight);

	FRetargetSkeleton MakeSkeleton(const FAnimationSet& Set, const FHumanoidMapping& Mapping);
	FRetargetPlan     MakePlan(const FRetargetSkeleton& Source, const FRetargetSkeleton& Target);
	// 소스 클립 → 대상 노드 채널 (회전: 활성 뼈 전부, 이동: Root/Hips). SampleRate Hz로 다시 샘플링
	FAnimationClip RetargetClip(const FAnimationClip& Clip, const FRetargetPlan& Plan, float SampleRate = 30.0f);
} // namespace AnimRetargetMath

// 리타기팅 소스 모델 (클립 + 스켈레톤 + 노티파이 .emeta)
struct FAnimSourceModel
{
	std::shared_ptr<const FAnimationSet>  Set;
	std::shared_ptr<const FModelMetadata> Metadata;
};

// 소스 모델/리타기팅 결과 공유 캐시 (엔진 DLL 전역 하나, 메인 스레드 전용)
class FAnimRetargetLibrary
{
public:
	static FAnimRetargetLibrary& Get();

	// 경로(Content 기준 또는 절대) → 소스 모델. 엔진은 Renderer(ModelLoader)가 시작 때 쿠킹 모델 로더를 등록한다
	using FLoader = std::function<bool(const std::string& AssetPath, FAnimSourceModel& Out)>;
	void SetLoader(FLoader InLoader) { Loader = std::move(InLoader); }

	// 실패하면 nullptr (경고 한 번 — 같은 경로는 Invalidate 전까지 다시 읽지 않는다)
	std::shared_ptr<const FAnimSourceModel> LoadSource(const std::string& AssetPath);
	// 테스트/도구: 메모리 소스 등록 (로더보다 먼저 찾는다)
	void AddSource(const std::string& AssetPath, FAnimSourceModel Source);

	// 소스 모델의 클립 전부를 대상에 맞춘 결과 (소스 경로 × TargetKey 캐시). 소스가 없거나 매핑이 모자라면(Hips 없음) nullptr
	std::shared_ptr<const std::vector<FAnimationClip>> GetRetargetedClips(const std::string& SourcePath, const std::string& TargetKey,
	                                                                     const FAnimationSet& Target, const FModelMetadata* TargetMetadata);

	// 경로 하나(소스 또는 대상)에 관련된 캐시 비우기 + 세대 증가
	void   Invalidate(const std::string& AssetPath);
	void   Invalidate();
	uint32 GetGeneration() const { return Generation; }

	// "<모델 경로>:<클립>" 나누기 — 모델 경로가 .glb/.gltf/.fbx로 끝나야 한다 (대소문자 무시)
	static bool        SplitQualifiedName(std::string_view Name, std::string& OutModel, std::string& OutClip);
	static std::string NormalizePath(std::string_view AssetPath); // 소문자 + '/' 구분 (캐시 키)

private:
	FLoader Loader;
	std::unordered_map<std::string, std::shared_ptr<const FAnimSourceModel>>               Sources;  // 실패도 nullptr로 기억
	std::unordered_map<std::string, std::shared_ptr<const FAnimSourceModel>>               Manual;
	std::unordered_map<std::string, std::shared_ptr<const std::vector<FAnimationClip>>>   Results;  // 키 = 소스|대상
	uint32 Generation = 1;
};

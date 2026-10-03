#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/AnimMontage.h"
#include "Scene/AnimNotify.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 스켈레탈/노드 애니메이션 데이터와 순수 계산 함수 (GPU·ECS 의존 없음, 단위 테스트 대상).
// 모든 값은 엔진 좌표계/단위(cm)로 변환된 상태다 (glTF 변환은 FGltfLoader 담당).

enum class EAnimationPath : uint8
{
	Translation,
	Rotation, // Values = 쿼터니언 (X, Y, Z, W)
	Scale,
};

enum class EAnimationInterpolation : uint8
{
	Linear, // 이동/스케일은 선형, 회전은 구면 선형
	Step,
};

// 노드 하나의 속성 하나에 대한 키프레임 트랙
struct FAnimationChannel
{
	int32                   Node          = -1; // 모델 노드 인덱스
	EAnimationPath          Path          = EAnimationPath::Translation;
	EAnimationInterpolation Interpolation = EAnimationInterpolation::Linear;
	std::vector<float>      Times;  // 오름차순 (초)
	std::vector<FVector4>   Values; // Times와 같은 개수. 이동/스케일은 xyz
};

struct FAnimationClip
{
	std::string                    Name;
	float                          Duration = 0.0f; // 마지막 키 시간
	std::vector<FAnimationChannel> Channels;
};

// 노드 로컬 트랜스폼 (부모 기준)
struct FNodePose
{
	FVector3 Translation;
	FQuat    Rotation;
	FVector3 Scale = FVector3::OneVector;

	FMatrix4x4 ToMatrix() const { return FMatrix4x4::MakeTransform(Translation, Rotation, Scale); }
};

struct FModelMetadata;

// 한 모델의 클립 묶음 + 스켈레톤(노드 계층) 정보. 인스턴스 간 공유(불변)
struct FAnimationSet
{
	std::vector<FAnimationClip> Clips;
	std::vector<int32>          NodeParents;   // 노드별 부모 인덱스 (-1 = 모델 루트)
	std::vector<FNodePose>      RestPose;      // 노드별 기본(임포트) 포즈
	std::vector<uint8>          AnimatedNodes; // 노드별: 어떤 클립이든 채널이 있으면 1
	std::vector<std::string>    NodeNames;     // 노드별 이름 (리타기팅 본 매핑/루트 모션 뼈 지정, 비어 있을 수 있다)
	std::vector<FMatrix4x4>     RestModelMatrices; // 노드별 기본 포즈 모델 공간 행렬

	// 루트 모션: 조상 중 이동 채널이 없는 최상위 이동 애니메이션 노드 (-1 = 없음)
	int32      RootMotionNode = -1;
	FMatrix4x4 RootMotionParentToModel; // 루트 모션 노드의 부모 공간 → 모델 공간 (기본 포즈 기준)
	// 클립별 루트 모션 노드 (그 클립 안에서 조상에 이동 채널이 없는 최상위 이동 채널 노드, -1 = 없음 — Scene/AnimRootMotion.h)
	std::vector<int32> ClipRootMotionNodes;

	// 리타기팅으로 덧붙인 클립 (Scene/AnimRetarget.h). 클립별 노티파이 출처 (.emeta) — nullptr이면 모델 자신(FAnimationRuntime::Metadata)
	std::vector<std::shared_ptr<const FModelMetadata>> ClipNotifySources;
	// 별칭 → 클립 번호 ("<소스 모델 경로>:<클립>" 형식의 정규 이름)
	std::vector<std::pair<std::string, int32>> ClipAliases;
	int32 OwnClipCount = 0; // 앞쪽 OwnClipCount개 = 모델 자신의 클립 (나머지는 리타기팅)

	// 이름(또는 별칭)으로 클립 검색 (-1 = 없음)
	int32 FindClip(std::string_view Name) const;
	int32 FindNode(std::string_view Name) const;
};

// 클립/파생 정보를 계산해 공유 가능한 세트로 만든다 (NodeNames는 비어도 된다)
std::shared_ptr<FAnimationSet> MakeAnimationSet(std::vector<FAnimationClip> Clips, std::vector<int32> NodeParents,
                                                      std::vector<FNodePose> RestPose, std::vector<std::string> NodeNames = {});

namespace AnimationMath
{
	// 채널을 Time에 샘플링 (범위 밖은 끝 키로 고정). 회전 채널은 정규화된 쿼터니언을 xyzw로 반환
	FVector4 SampleChannel(const FAnimationChannel& Channel, float Time);

	// 클립의 채널을 샘플링해 InOutPose의 해당 노드 속성을 덮어쓴다 (없는 노드는 그대로)
	void SampleClip(const FAnimationClip& Clip, float Time, std::vector<FNodePose>& InOutPose);

	// A → B 보간 (Alpha 0 = A, 1 = B). 이동/스케일 선형, 회전 구면 선형
	FNodePose BlendPose(const FNodePose& A, const FNodePose& B, float Alpha);
	void      BlendPoses(const std::vector<FNodePose>& A, const std::vector<FNodePose>& B, float Alpha, std::vector<FNodePose>& Out);

	// 재생 시간 진행. 루프면 [0, Duration)로 감고 bOutWrapped = true, 아니면 [0, Duration]에 고정
	float AdvanceTime(float Time, float Delta, float Duration, bool bLoop, bool& bOutWrapped);

	// 크로스페이드에서 새 클립의 가중치 [0, 1] (Duration <= 0이면 즉시 1). 부드러운 가감속(smoothstep)
	float ComputeCrossfadeWeight(float Elapsed, float Duration);

	// 노드별 로컬 포즈 → 모델 공간 행렬 (부모 순서와 무관하게 계산)
	void ComputeModelMatrices(const std::vector<FNodePose>& Pose, const std::vector<int32>& Parents, std::vector<FMatrix4x4>& OutMatrices);

	// 루트 모션 이동량 (노드 부모 공간). 루프 경계를 넘으면 끝까지 + 처음부터 합산
	FVector3 ComputeRootMotionDelta(const FAnimationChannel& TranslationChannel, float PreviousTime, float NewTime, bool bWrapped,
	                                float Duration);

	// 노드별 모델 공간 회전 (부모 모델 회전 * 로컬 회전, 균등 스케일 가정 — 부모 순서와 무관)
	void ComputeModelRotations(const std::vector<FNodePose>& Pose, const std::vector<int32>& Parents, std::vector<FQuat>& OutRotations);
} // namespace AnimationMath

// 루트 모션 이동량 (규칙은 Scene/AnimRootMotion.h). 모델 공간: Translation은 수평(Z = 0), Yaw는 Z축 회전 (라디안, + = Forward → Right)
//   적용 = 이동 먼저(현재 방향 기준) → 회전. 합성/가중은 RootMotionMath
struct FRootMotionDelta
{
	FVector3 Translation;
	float    Yaw = 0.0f;

	bool IsZero() const { return Translation.LengthSquared() <= 1.0e-12f && Yaw == 0.0f; }
};

// FAnimationComponent의 런타임 상태 (직렬화 제외). 모델 인스턴스화 시 FModelLoader가 채운다
struct FAnimationRuntime
{
	std::shared_ptr<const FAnimationSet> Set;
	std::vector<FEntity>                 NodeEntities; // 모델 노드 인덱스 → 엔티티

	// 리타기팅 (Scene/AnimRetarget.h): BaseSet = 모델 자신의 세트, Set = 소스 모델 클립을 덧붙인 세트 (소스가 없으면 BaseSet 그대로).
	// 누군가 Set을 바꾸면(모델 다시 인스턴스화) BuiltSet과 달라지므로 그 Set을 새 BaseSet으로 삼아 다시 만든다
	std::shared_ptr<const FAnimationSet> BaseSet;
	const FAnimationSet*                 BuiltSet = nullptr;
	std::string                          RetargetKey;      // 만든 세트의 소스 목록 + 라이브러리 세대
	std::vector<std::string>             RequestedSources; // Play/PlayMontage가 "<모델>:<클립>" 이름으로 요청한 소스 모델 (계속 유지)

	// 루트 모션: 직전 갱신에서 추출한 양 (모델 공간 — 적용 규칙은 Scene/AnimRootMotion.h)
	FRootMotionDelta RootMotion;

	int32       CurrentClip = -1;
	float       CurrentTime = 0.0f;
	int32       PreviousClip = -1; // 크로스페이드 중 이전 클립
	float       PreviousTime = 0.0f;
	float       BlendElapsed  = 0.0f;
	float       BlendDuration = 0.0f;
	float       PendingBlendTime = -1.0f; // FAnimationSystem::Play가 지정한 블렌드 시간 (음수 = 컴포넌트 값)
	std::string ActiveClipName;           // 적용 중인 Clip 문자열 (변경 감지용)
	bool        bWarnedUnknownClip = false;

	// 노티파이 (.emeta 공유 데이터, 규칙은 Scene/AnimNotify.h)
	std::shared_ptr<const FModelMetadata> Metadata;
	FAnimNotifyTrack                      Notify;           // 클립 재생(그래프 아님)의 노티파이 진행 상태
	std::vector<FAnimNotifyEvent>         PendingNotifies;  // 직전 갱신에서 발생 (다음 갱신 시작에 비움)
	bool                                  bPhysicsPose = false; // 래그돌이 뼈를 구동하는 중: 애니메이션 갱신을 건너뛴다 (Physics/Ragdoll.h)

	// 몽타주 (Scene/AnimMontage.h, 로컬 전용). 시작 순서대로 덮어 섞는다
	std::vector<FAnimMontageInstance> Montages;
	std::vector<FAnimMontageEvent>    PendingMontageEvents; // 직전 갱신에서 끝난 몽타주 (다음 갱신 시작에 비움)

	std::vector<FNodePose> PoseScratch;
	std::vector<FNodePose> BlendScratch;
	std::vector<FNodePose> MontageScratch;
	std::vector<uint8>     IkTouched;      // IK가 바꾼 적 있는 노드 (채널이 없어도 계속 기록)
	std::vector<FMatrix4x4> IkMatrices;
	std::vector<FQuat>      IkRotations;
	std::vector<FAnimNotifyHit> HitScratch;

	// 갱신 빈도 LOD (Scene/AnimUpdateRate.h): 갱신 호출 횟수, 건너뛰며 모은 시간 (다음 평가가 한 번에 진행), 한 번이라도 평가했는지
	uint32 UpdateRateTick       = 0;
	float  UpdateRatePending    = 0.0f;
	bool   bUpdateRateEvaluated = false;
	uint8  UpdateRateInterval   = 1; // 직전 갱신이 고른 간격 (확인용)
};

#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Scene/Animation.h"

class FScene;

// 루트 모션 (언리얼 Root Motion식). FAnimationSystem이 포즈를 만들며 추출하고, 갱신 끝에 엔티티 이동으로 옮긴다.
//
// 모드 ERootMotionMode (FAnimationComponent::RootMotionMode — 씬 JSON에 번호로 저장되므로 끝에만 추가):
//   None         = 추출하지 않는다 (포즈 그대로). 단, 이전 설정 bRootMotion(RootMotion)이 켜져 있으면 All과 같다.
//   MontagesOnly = 몽타주 클립만 추출 (클립 재생/그래프 포즈에는 이동이 그대로 남는다).
//   All          = 클립 재생(크로스페이드 포함)·그래프(기본/추가 레이어)·몽타주 전부.
// 루트 노드: 컴포넌트 RootMotionBone(노드 이름)이 있으면 그 노드, 없으면 클립마다 FAnimationSet::ClipRootMotionNodes
//   (그 클립에서 조상에 이동 채널이 없는 최상위 이동 채널 노드 — 전용 root 뼈가 있으면 root, 없으면 보통 Hips).
// 추출 (RootMotionMath): 루트 노드의 모델 공간 위치 P(t)와, bRootMotionRotation이면 시작 회전 대비 Z축(위) 회전 Y(t)
//   (twist 분해 — 회전 추출이 꺼져 있으면 Y ≡ 0). 구간 a → b = 이동 RotZ(-Y(a)) × (P(b) - P(a))의 XY, 회전 Y(b) - Y(a) ([-π, π]).
//   루프 경계를 넘으면 (a → 끝)과 (시작 → b)를 합성(Compose)한다 — 역재생은 (a → 시작) 다음 (끝 → b). 수직(Z) 이동은 포즈에 남는다.
// 제자리화 (RemoveFromPose): 샘플한 포즈의 루트 노드를 모델 공간 XY = P(0)의 XY로 고정하고, 회전 추출이면 Z축 회전 -Y(t)를 곱해
//   시작 방향으로 되돌린다. 루프 끝 → 시작에서 포즈의 XY가 같은 자리라 튀지 않고, 이동량도 경계에서 이어진다.
// 섞기: 클립 크로스페이드 = Lerp(이전, 현재, 가중치), 그래프 기여 = Σ 가중치 × 이동량, 추가 레이어·몽타주 = 시작 순서대로
//   Lerp(아래, 위, 가중치 × 루트 노드의 마스크 가중치) (프레임 이동량이 작아 회전도 각도 선형 보간).
// 적용 (FAnimationSystem::Update 끝, 이동량 = 모델 공간 = 애니메이션 엔티티 로컬 공간):
//   수신자(FAnimationSystem::SetRootMotionReceiver)가 받으면 그쪽 — Physics가 자신 또는 조상의 FCharacterMovementComponent에 쌓아 두고
//   다음 게임플레이 틱의 무브가 입력 대신 "루트 모션 속도"로 이동한다(충돌·바닥 유지, 회전은 캐릭터 방향 규칙을 따름 — CharacterMovement.h).
//   받지 않으면 애니메이션 엔티티의 로컬 트랜스폼을 직접: 위치 += 회전 × (이동 × 스케일), 회전 = 회전 × RotZ(Yaw).
//   래그돌 중(bPhysicsPose)에는 추출하지 않는다. 결과는 FAnimationRuntime::RootMotion에도 남는다 (편집기/테스트 확인용).

enum class ERootMotionMode : int32
{
	None,
	MontagesOnly,
	All,
};

// 클립 하나의 루트 노드 샘플러 (그 클립 채널 포인터를 들고 있으므로 클립보다 오래 쓰지 않는다)
struct FRootMotionTrack
{
	int32                    Node        = -1;
	const FAnimationChannel* Translation = nullptr;
	const FAnimationChannel* Rotation    = nullptr;
	FNodePose                Rest;            // 채널이 없을 때 쓰는 노드 기본 로컬 포즈
	FMatrix4x4               ParentToModel;   // 부모 공간 → 모델 공간 (기본 포즈)
	FQuat                    ParentRotation;  // 부모의 모델 공간 회전 (기본 포즈)
	FVector3                 StartPosition;   // 모델 공간 P(0)
	FQuat                    StartRotation;   // 모델 공간 회전 (시각 0)
	bool                     bRotation = false;

	bool IsValid() const { return Node >= 0 && (Translation != nullptr || (bRotation && Rotation != nullptr)); }
};

namespace RootMotionMath
{
	// 각도 [-π, π]로 감기
	float NormalizeAngle(float Radians);
	// Z축 twist 각도 (라디안) — 회전을 Z축 회전(twist) × 나머지(swing)로 나눈 twist
	float ExtractYaw(const FQuat& Rotation);
	FQuat MakeYawRotation(float Radians);

	// Set의 클립 Clip에서 Node(-1이면 그 클립의 ClipRootMotionNodes)의 샘플러. 노드가 없으면 IsValid() == false
	FRootMotionTrack MakeTrack(const FAnimationSet& Set, int32 Clip, int32 Node, bool bRotation);

	// 시각 Time의 모델 공간 위치와 시작 대비 Yaw (회전 추출이 꺼져 있으면 0)
	void SampleModel(const FRootMotionTrack& Track, float Time, FVector3& OutPosition, float& OutYaw);

	// 구간 From → To 한 번 (감기 없음)
	FRootMotionDelta ComputeSegment(const FRootMotionTrack& Track, float From, float To);
	// 재생 한 단계: PreviousTime → NewTime, Delta = 부호 있는 진행량(초), bWrapped = 루프 경계를 넘음
	FRootMotionDelta ComputeDelta(const FRootMotionTrack& Track, float PreviousTime, float NewTime, float Delta, bool bWrapped, float Duration);

	// First 다음 Then (Then은 First가 끝난 방향 기준)
	FRootMotionDelta Compose(const FRootMotionDelta& First, const FRootMotionDelta& Then);
	FRootMotionDelta Lerp(const FRootMotionDelta& A, const FRootMotionDelta& B, float Alpha);
	void             AddWeighted(FRootMotionDelta& Accumulator, const FRootMotionDelta& Delta, float Weight);

	// 샘플한 루트 노드 로컬 포즈(시각 Time)를 제자리로
	void RemoveFromPose(const FRootMotionTrack& Track, float Time, FNodePose& InOutRootPose);

	// 엔티티 로컬 트랜스폼에 직접 적용 (위치 += 회전 × (이동 × 스케일), 회전 = 회전 × RotZ(Yaw))
	void ApplyToTransform(const FRootMotionDelta& Delta, FVector3& InOutPosition, FQuat& InOutRotation, const FVector3& Scale);
} // namespace RootMotionMath

// 루트 모션 수신자: 받으면 true (그러면 엔티티 트랜스폼을 직접 움직이지 않는다).
//   AnimEntity = FAnimationComponent가 있는 엔티티, WorldTranslation = 월드 공간 이동 (cm), WorldYaw = 라디안, DeltaSeconds = 이번 갱신 시간
using FRootMotionReceiver = bool (*)(FScene& Scene, FEntity AnimEntity, const FVector3& WorldTranslation, float WorldYaw, float DeltaSeconds);

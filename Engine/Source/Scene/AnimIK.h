#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>
#include <vector>

struct FAnimationSet;

// 애니메이션 IK (Phase 42-4): 최종 포즈의 마지막 단계 (기본 레이어 → 추가 레이어 → 몽타주 → IK → 노드 기록, Scene/AnimGraph.h).
//   계산은 모델 공간(모델 루트 = FAnimationComponent 엔티티 기준, glTF 노드 단위 — Fox처럼 루트 Scale이 있으면 모델 공간도 그만큼 크다).
//   월드 값(바닥 점, 법선, 시선 목표)은 항상 모델 루트 월드 행렬의 역행렬로 바꿔 쓴다 (점은 TransformPosition, 방향/길이는 TransformVector).
//   회전 체인은 균등 스케일을 가정한다 (모델 회전 = 부모 모델 회전 * 로컬 회전). IK가 바꾼 노드는 채널이 없어도 기록된다.
//   래그돌(FAnimationRuntime::bPhysicsPose) 중에는 애니메이션 갱신 자체를 건너뛰므로 IK도 꺼지고, 보정값/가중치는 0으로 돌아간다.
// 발 IK (FFootIkComponent, 모델 루트에): 발(끝 뼈)마다 2본 체인 (끝 뼈의 부모 = 무릎, 조부모 = 허벅지).
//   바닥 탐색은 Scene이 아니라 World가 한다 (Scene → Physics 의존 금지): FGameWorld::UpdateFootIkProbes가 게임플레이 틱의 물리·UpdateTransforms 뒤에
//   직전 애니메이션의 발 위치(IK 전, Runtime.Feet[].ProbeModelPosition)에서 아래로 기존 FPhysicsSystem::Raycast를 쏘아 Runtime에 넣는다
//   (자기 몸(모델 루트의 조상/자손)에 맞으면 1cm 아래에서 다시 쏜다, 최대 3번). 탐색 결과는 다음 애니메이션 갱신이 쓴다 (한 프레임 늦음),
//   ProbeMaxAge(0.25초)보다 오래되면 맞지 않은 것으로 보고 보정이 0으로 돌아간다 (에디터 편집 중·물리 없음).
//   발 보정 = clamp(바닥 높이 - 모델 루트 월드 Z, ±MaxAdjust) (cm, 월드 위 방향) → InterpSpeed로 부드럽게. 골반 = min(0, 발 보정들) (내리기만).
//   골반을 내린 뒤 각 발을 (애니메이션 발 위치 + 위 × 보정)으로 2본 IK, 무릎 방향 = KneeDirection(모델 공간, 0이면 애니메이션의 무릎 방향).
//   bAlignToGround면 발 뼈를 바닥 법선에 맞춰 기울인다 (MaxAlignAngle까지). Weight가 보정/기울기에 곱해진다.
// 시선 (FLookAtComponent, 모델 루트에): Bones(위 → 아래, 쉼표) 마지막 뼈의 앞 방향을 목표로. 앞 방향 = 기본 포즈에서 ForwardAxis(모델 공간)와
//   같은 뼈 로컬 축. 회전 = 애니메이션 앞 방향 → 목표 방향, MaxAngle(도)에서 자르고 × 가중치, 뼈 개수로 나눠 위 뼈부터 차례로 (모델 공간).
//   목표 = Lua SetLookAtTarget(점/엔티티)이 있으면 그것, 없으면 Target 엔티티. 목표가 없거나 꺼지면 가중치가 BlendSpeed로 0이 된다.

namespace AnimIKMath
{
	// From → To 최소 회전 (길이 무관). 반대 방향이면 From에 수직인 축으로 180도
	FQuat FromToRotation(const FVector3& From, const FVector3& To);

	struct FTwoBoneResult
	{
		FQuat    RootDelta;            // 허벅지 모델 회전 앞에 곱한다 (새 모델 회전 = RootDelta * 이전)
		FQuat    MidDelta;             // 무릎: 새 모델 회전 = MidDelta * RootDelta * 이전
		FVector3 EndPosition;          // 풀이 뒤 끝 뼈 위치
		bool     bReachable = true;    // 목표가 체인 길이 안
	};
	// 2본 IK (모델 공간 위치). Pole = 무릎이 향할 방향 (0이면 지금 무릎 방향 유지). 닿지 않으면 목표 쪽으로 뻗는다 (길이 × 0.9999)
	FTwoBoneResult SolveTwoBone(const FVector3& Root, const FVector3& Mid, const FVector3& End, const FVector3& Target, const FVector3& Pole);

	// 시선 회전: CurrentForward → Desired를 MaxAngle(라디안)에서 자르고 Weight(0~1)만큼. Desired가 0이면 항등
	FQuat ComputeLookAtDelta(const FVector3& CurrentForward, const FVector3& Desired, float MaxAngleRadians, float Weight);

	// 지수 평활: Current → Target (Speed = 1/초, DeltaSeconds <= 0이면 그대로)
	float SmoothTowards(float Current, float Target, float Speed, float DeltaSeconds);

	// 쉼표로 나눈 뼈 이름 목록 (앞뒤 공백 제거, 빈 항목 제외)
	std::vector<std::string> SplitBoneList(const std::string& Text);
} // namespace AnimIKMath

// 발 하나의 런타임 (World와 애니메이션 시스템이 주고받는다)
struct FFootIkFoot
{
	int32    End = -1, Mid = -1, Root = -1; // 노드 번호
	FVector3 ProbeModelPosition;            // 직전 애니메이션의 IK 전 끝 뼈 위치 (모델 공간) — World가 탐색 위치로 쓴다
	bool     bHasProbePosition = false;
	bool     bHit = false;                  // World 탐색 결과
	FVector3 HitPoint;                      // 월드 (cm)
	FVector3 HitNormal = FVector3::UpVector;
	float    Offset = 0.0f;                 // 평활된 높이 보정 (cm, 월드)
	FVector3 Normal = FVector3::UpVector;   // 평활된 바닥 법선 (월드)
};

struct FFootIkRuntime
{
	static constexpr float ProbeMaxAge = 0.25f;

	std::vector<FFootIkFoot> Feet;
	int32                    Pelvis       = -1;
	float                    PelvisOffset = 0.0f; // 평활된 골반 보정 (cm, 월드, <= 0)
	float                    ProbeAge     = 1.0e9f; // 마지막 World 탐색 뒤 (초)
	const FAnimationSet*     ResolvedSet = nullptr; // 뼈 번호를 찾은 세트 + 문자열 (바뀌면 다시 찾는다)
	std::string              ResolvedFeet;
	std::string              ResolvedPelvis;

	void ResetBlend(); // 보정 0 (래그돌/끔)

	FFootIkRuntime() = default;
	FFootIkRuntime(const FFootIkRuntime&) {}
	FFootIkRuntime& operator=(const FFootIkRuntime&) { return *this; }
	FFootIkRuntime(FFootIkRuntime&&) noexcept            = default;
	FFootIkRuntime& operator=(FFootIkRuntime&&) noexcept = default;
};

// 발 IK 설정 (모델 루트 = FAnimationComponent 엔티티에). 규칙은 머리 주석
struct FFootIkComponent
{
	bool        bEnabled       = true;
	std::string FootBones;                 // 끝 뼈 이름 (쉼표). 각 끝 뼈의 부모/조부모가 체인
	std::string PelvisBone;                // 비면 골반 보정 없음
	float       TraceUp        = 50.0f;    // cm, 발 위에서 쏘기 시작
	float       TraceDown      = 60.0f;    // cm, 발 아래로
	float       MaxAdjust      = 30.0f;    // cm, 발 보정 한계
	float       InterpSpeed    = 12.0f;    // 1/초
	float       Weight         = 1.0f;
	bool        bAlignToGround = true;
	float       MaxAlignAngle  = 30.0f;    // 도
	FVector3    KneeDirection;             // 모델 공간 무릎 방향 (0 = 애니메이션 무릎 방향)

	FFootIkRuntime Runtime;
};

struct FLookAtRuntime
{
	bool     bHasScriptTarget = false; // Lua SetLookAtTarget
	FVector3 ScriptTarget;             // 월드 점 (ScriptTargetEntity가 유효하면 그 위치)
	FEntity  ScriptTargetEntity;
	float    CurrentWeight = 0.0f;
	FVector3 LastTarget;               // 마지막 목표 (월드) — 목표가 사라져 가중치가 빠지는 동안 쓴다
	bool     bHasLastTarget = false;

	std::vector<int32>   Bones;      // 위 → 아래
	FVector3             LocalForward = FVector3::ForwardVector; // 마지막 뼈 로컬 앞 축
	const FAnimationSet* ResolvedSet = nullptr;
	std::string          ResolvedBones;
	FVector3             ResolvedForwardAxis;

	FLookAtRuntime() = default;
	FLookAtRuntime(const FLookAtRuntime&) {}
	FLookAtRuntime& operator=(const FLookAtRuntime&) { return *this; }
	FLookAtRuntime(FLookAtRuntime&&) noexcept            = default;
	FLookAtRuntime& operator=(FLookAtRuntime&&) noexcept = default;
};

// 시선 설정 (모델 루트에). 규칙은 머리 주석
struct FLookAtComponent
{
	bool        bEnabled    = true;
	std::string Bones;                                  // 위 → 아래 (쉼표), 마지막 뼈가 목표를 본다
	FEntity     Target;                                 // Lua 목표가 없을 때
	FVector3    ForwardAxis = FVector3::ForwardVector;  // 기본 포즈에서 캐릭터 앞 방향 (모델 공간)
	float       MaxAngle    = 70.0f;                    // 도
	float       Weight      = 1.0f;
	float       BlendSpeed  = 6.0f;                     // 1/초

	FLookAtRuntime Runtime;
};

#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>

// 캐릭터 이동 (언리얼 CharacterMovementComponent 역할) — Jolt CharacterVirtual(쿼리 기반 캡슐)로 걷기/점프/중력/경사 제한/계단/벽 미끄러짐.
//   엔티티 트랜스폼 위치 = 캡슐 중심. 콜라이더·강체 컴포넌트는 쓰지 않는다 (있어도 캐릭터가 우선).
//   스크립트/게임 모듈은 이동 방향(AddMovementInput)과 점프(Jump)만 넘기고, 실제 이동은 "무브" 단위로 FPhysicsSystem::SimulateCharacter가 한다.
//   멀티플레이: 소유 클라이언트가 무브를 즉시 적용(예측)하고 서버로 보내며, 서버가 같은 무브로 다시 계산해 보정한다 (FGameWorld).
struct FCharacterMovementComponent
{
	float MaxWalkSpeed      = 450.0f;  // cm/s
	float JumpZVelocity     = 520.0f;  // cm/s (점프 순간 위쪽 속도)
	float AirControl        = 0.35f;   // 0~1: 공중에서 방향을 바꾸는 정도 (0 = 못 바꿈)
	float GravityScale      = 1.0f;    // 프로젝트 설정 중력 × 배율
	float CapsuleRadius     = 35.0f;   // cm
	float CapsuleHalfHeight = 55.0f;   // cm, 원기둥 부분 절반 (전체 높이 = 2 × (HalfHeight + Radius))
	float MaxStepHeight     = 35.0f;   // cm, 오를 수 있는 계단 높이
	float MaxSlopeAngle     = 50.0f;   // 도, 이보다 가파르면 미끄러진다
	float Mass              = 80.0f;   // kg (밀기/밀리기)
	float PushForce         = 4000.0f; // N, 부딪힌 동적 물체를 미는 최대 힘
	bool  bFaceControlYaw   = true;    // 몸이 시점 방향(yaw)을 본다. 끄면 이동 방향을 본다
	bool  bClientPrediction = true;    // 멀티플레이: 소유 클라이언트가 입력 즉시 미리 움직인다 (프로젝트 설정 네트워크 → 클라이언트 예측도 켜져 있어야).
	                                   // 끄면 무브를 보내기만 하고 서버 결과를 보간해 보여 준다 (반응은 늦지만 보정이 없다)
	std::string Layer;                 // 충돌 레이어 이름 (캡슐과 내부 바디 모두, 비면 Default — Core/Settings/CollisionSettings.h)
};

// 무브 하나 = 한 프레임 입력 (네트워크로 보내고 다시 적용하는 단위). 같은 무브 → 같은 결과
struct FCharacterMove
{
	uint32   Sequence     = 0;     // 소유 클라이언트가 붙이는 순번 (재조정 기준)
	float    DeltaSeconds = 0.0f;  // 0~MaxMoveDeltaSeconds로 잘라 적용
	FVector2 Input;                // 월드 XY 이동 방향 (길이 ≤ 1)
	float    Yaw   = 0.0f;         // 도 (bFaceControlYaw일 때 몸 방향)
	bool     bJump = false;        // 바닥에 있으면 점프

	static constexpr float MaxMoveDeltaSeconds = 0.1f; // 서버가 받은 dt 상한 (느린 프레임/조작 방지)
};

// 캐릭터 상태 (서버 → 소유 클라이언트 보정, 재조정 시작점)
struct FCharacterState
{
	FVector3 Position; // cm (캡슐 중심)
	FVector3 Velocity; // cm/s
	bool     bGrounded = false;
};

namespace CharacterMovementMath
{
	// 무브를 적용하기 전 속도 계산 (Jolt 이동 전 단계 — 순수 함수, 테스트 대상)
	//   바닥: 수평 = 입력 × 최대 속도, 수직 = 0 (점프면 JumpZVelocity)
	//   공중: 수평은 AirControl만큼 입력 쪽으로 가속, 수직은 유지
	//   항상 중력 × 배율 × dt를 더한다 (바닥에서는 Jolt가 바닥에 붙여 둔다)
	FVector3 ComputeVelocity(const FCharacterMovementComponent& Movement, const FVector3& CurrentVelocity, bool bGrounded, const FCharacterMove& Move,
	                         float GravityZ, bool& bOutJumped);

	// 입력 방향 정리: 길이 1로 자른다 (대각선이 빠르지 않게)
	FVector2 ClampInput(const FVector2& Input);
} // namespace CharacterMovementMath

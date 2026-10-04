#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>

// 캐릭터 이동 (언리얼 CharacterMovementComponent 역할) — Jolt CharacterVirtual(쿼리 기반 캡슐)로 걷기/점프/중력/경사 제한/계단/벽 미끄러짐.
//   엔티티 트랜스폼 위치 = 캡슐 중심. 콜라이더·강체 컴포넌트는 쓰지 않는다 (있어도 캐릭터가 우선).
//   스크립트/게임 모듈은 이동 방향(AddMovementInput)과 점프(Jump)만 넘기고, 실제 이동은 "무브" 단위로 FPhysicsSystem::SimulateCharacter가 한다.
//   멀티플레이: 소유 클라이언트가 무브를 즉시 적용(예측)하고 서버로 보내며, 서버가 같은 무브로 다시 계산해 보정한다 (FGameWorld).
//   넉백/발사 (UE LaunchCharacter식 — 2D 이동기 CharacterMovement2D.h와 같은 규칙): LaunchCharacter(속도, 덮어쓰기 XY, 덮어쓰기 Z) = 다음 무브 처음에
//     속도를 더하거나 덮어쓴다 (수평 XY는 한 묶음). 발사한 무브는 공중 규칙으로 계산하고 바닥에 붙이지 않는다 — 위로 향하면 바닥을 떠난다.
//     수평만 발사하면 다음 무브에 다시 바닥이면 걷기 규칙(수평 = 입력)으로 돌아가므로 밀려나는 효과는 AddKnockback을 쓴다.
//     AddKnockback(속도, 경직 초) = XY는 덮어쓰고 Z는 0이 아닐 때만 덮어쓰며, 경직 동안 입력(이동·점프)을 무시하고 수평 속도를
//     KnockbackDeceleration으로 줄인다 (바닥·공중 공통, 공중 조작도 없음). 둘 다 무브 필드(bLaunch …)로 들어가 같은 무브 → 같은 결과
//     (예측·재조정 포함, 경직 타이머는 상태에 있다). 누가 어느 무브에 넣는가는 World/GameWorldCharacter.cpp 머리 주석
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
	float KnockbackDeceleration = 1500.0f; // cm/s², 넉백 경직(AddKnockback) 중 수평 감속 (바닥·공중 공통)

	// 루트 모션 (Scene/AnimRootMotion.h — 런타임 전용, 리플렉션/직렬화 제외): 자신 또는 자손의 애니메이션이 추출한 월드 이동을 쌓아 두면
	// 다음 무브가 가져가 "루트 모션 속도"(쌓인 이동 ÷ 쌓인 시간)로 움직인다. 0.25초 넘게 아무도 가져가지 않으면 오래된 것은 버린다
	FVector3 PendingRootMotion;
	float    PendingRootMotionSeconds = 0.0f;
};

// 무브 하나 = 한 프레임 입력 (네트워크로 보내고 다시 적용하는 단위). 같은 무브 → 같은 결과
//   루트 모션 규칙 (멀티플레이 포함):
//     루트 모션은 애니메이션을 평가하고 그 캐릭터를 시뮬레이션하는 프로세스에서만 무브에 실린다 — 조종 주체(IsLocallyControlled)가
//     ConsumePendingMove 뒤 컴포넌트의 쌓인 루트 모션을 bRootMotion/RootMotionVelocity로 넣는다. 그 무브는 입력 대신 그 수평 속도로
//     이동하고(바닥이면 수직 0·점프 무시, 공중이면 중력 유지), 몸 방향 규칙(Yaw)은 그대로다 (루트 모션 회전은 캐릭터에 적용하지 않음).
//     - Standalone/리슨 서버의 자기 캐릭터·서버 소유 캐릭터(AI): 서버가 몽타주를 재생 → 그대로 적용 (서버 권한)
//     - 원격 플레이어 캐릭터 (예측): 소유 클라이언트가 몽타주를 재생해 루트 모션 무브를 예측 적용하고 무브와 함께 보낸다(속도 포함).
//       서버는 받은 속도를 MaxRootMotionSpeed로 잘라 같은 무브를 적용하므로 보정이 거의 없다. 서버도 같은 몽타주를 재생하면(Both 스크립트)
//       서버의 애니메이션은 루트 모션을 쌓지만 원격 캐릭터는 받은 무브로만 움직이므로 쌓인 것은 쓰이지 않고 버려진다.
//     - 다른 클라이언트에서 보이는 캐릭터: 복제 보간 위치를 따른다 (루트 모션은 포즈에서만 빠짐 — 같은 몽타주를 재생해야 발이 맞는다)
//     - 전용 서버는 모델/애니메이션을 인스턴스화하지 않으므로 서버 소유 캐릭터의 루트 모션은 없다 (후속: 서버용 애니메이션 평가)
struct FCharacterMove
{
	uint32   Sequence     = 0;     // 소유 클라이언트가 붙이는 순번 (재조정 기준)
	float    DeltaSeconds = 0.0f;  // 0~MaxMoveDeltaSeconds로 잘라 적용
	FVector2 Input;                // 월드 XY 이동 방향 (길이 ≤ 1)
	float    Yaw   = 0.0f;         // 도 (bFaceControlYaw일 때 몸 방향)
	bool     bJump = false;        // 바닥에 있으면 점프
	bool     bRootMotion = false;  // 루트 모션 무브: Input 대신 RootMotionVelocity로 이동
	FVector2 RootMotionVelocity;   // 월드 XY cm/s
	// 넉백/발사 (LaunchCharacter/AddKnockback — 컴포넌트 위 주석): 무브 처음에 속도에 더하거나(덮어쓰기 꺼짐) 덮어쓴다, 경직 초 (0 = 없음)
	bool     bLaunch           = false;
	bool     bLaunchOverrideXY = false;
	bool     bLaunchOverrideZ  = false;
	FVector3 LaunchVelocity;            // 월드 cm/s
	float    StunSeconds       = 0.0f;

	static constexpr float MaxMoveDeltaSeconds = 0.1f;    // 서버가 받은 dt 상한 (느린 프레임/조작 방지)
	static constexpr float MaxRootMotionSpeed  = 3000.0f; // cm/s, 루트 모션 속도 상한 (서버 검증)
	static constexpr float MaxLaunchSpeed      = 1.0e5f;  // cm/s, 발사 속도 성분 상한 (서버 검증 — 2D와 같음)
	static constexpr float MaxStunSeconds      = 10.0f;
};

// 캐릭터 상태 (서버 → 소유 클라이언트 보정, 재조정 시작점)
struct FCharacterState
{
	FVector3 Position; // cm (캡슐 중심)
	FVector3 Velocity; // cm/s
	bool     bGrounded = false;
	float    StunTimer = 0.0f; // > 0 = 넉백 경직 (입력 무시)

	bool IsStunned() const { return StunTimer > 0.0f; }
};

namespace CharacterMovementMath
{
	// 무브를 적용하기 전 속도 계산 (Jolt 이동 전 단계 — 순수 함수, 테스트 대상)
	//   바닥: 수평 = 입력 × 최대 속도, 수직 = 0 (점프면 JumpZVelocity)
	//   공중: 수평은 AirControl만큼 입력 쪽으로 가속, 수직은 유지
	//   루트 모션 무브: 수평 = RootMotionVelocity (바닥/공중 모두), 바닥이면 수직 0이고 점프하지 않는다
	//   항상 중력 × 배율 × dt를 더한다 (바닥에서는 Jolt가 바닥에 붙여 둔다)
	//   발사(bLaunch, 가장 먼저): 수평 XY·수직 Z 성분마다 덮어쓰기면 LaunchVelocity, 아니면 더한다 (유한하지 않으면 0, MaxLaunchSpeed로 자름).
	//     발사한 무브는 공중으로 계산한다(바닥 규칙·점프·바닥 붙이기 없음 — bOutAirborne). 경직 = max(남은 경직, StunSeconds)
	//   경직(InOutStunTimer > 0, 이 무브 dt만큼 준다): 입력·점프·루트 모션을 무시하고 수평 속도를 KnockbackDeceleration으로 0 쪽으로 줄인다
	//     (바닥이면 수직 0, 공중이면 중력)
	FVector3 ComputeVelocity(const FCharacterMovementComponent& Movement, const FVector3& CurrentVelocity, bool bGrounded, const FCharacterMove& Move,
	                         float GravityZ, float& InOutStunTimer, bool& bOutJumped, bool& bOutAirborne);
	// 경직·발사 없는 무브용 (경직 상태를 쓰지 않는 호출 — 테스트·발사 없는 경로)
	FVector3 ComputeVelocity(const FCharacterMovementComponent& Movement, const FVector3& CurrentVelocity, bool bGrounded, const FCharacterMove& Move,
	                         float GravityZ, bool& bOutJumped);
	// 발사 하나를 무브(쌓인 발사)에 합친다 — 성분마다 덮어쓰기면 대체, 아니면 합 (덮어쓰기 표시는 남는다), 경직은 긴 쪽
	void CombineLaunch(FCharacterMove& Into, const FVector3& Velocity, bool bOverrideXY, bool bOverrideZ, float StunSeconds);
	// 받은 무브의 발사 값 정리 (유한하지 않으면 false, 성분을 MaxLaunchSpeed·경직을 MaxStunSeconds로 자른다)
	bool SanitizeLaunch(FCharacterMove& Move);

	// 입력 방향 정리: 길이 1로 자른다 (대각선이 빠르지 않게)
	FVector2 ClampInput(const FVector2& Input);
	// 루트 모션 속도 정리: 유한하지 않으면 0, 길이 MaxRootMotionSpeed로 자른다
	FVector2 ClampRootMotionVelocity(const FVector2& Velocity);
	// 쌓인 루트 모션을 무브에 옮기고 비운다 (쌓인 것이 없으면 그대로)
	void ConsumeRootMotion(FCharacterMovementComponent& Movement, FCharacterMove& Move);
} // namespace CharacterMovementMath

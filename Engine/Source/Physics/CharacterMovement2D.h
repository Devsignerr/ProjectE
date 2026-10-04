#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>

// 2D 캐릭터 이동기 (Phase 56 후속) — 플랫포머(달리기·점프·n단 점프·대시·원웨이 내려가기·경사)와 탑다운(중력 0, 8방향) 공용.
// 평면 규약은 Physics/Physics2DMath.h (평면 = 월드 X·Z, 중력 -Z, 깊이 Y 유지). 3D FCharacterMovementComponent와 같은 구조:
//   스크립트/게임 모듈은 입력(AddMovementInput/Jump/StopJumping/Dash/DropDown)만 넘기고, 실제 이동은 "무브" 단위로
//   FCharacterMovement2DSystem::SimulateCharacter가 한다 (같은 상태 + 같은 무브 + 같은 월드 → 같은 결과 — 멀티플레이 예측/재조정의 기준).
//   한 무브 = CharacterMovement2DMath::BeginMove(속도·타이머·점프·대시, 순수 함수) → Box2D 캐릭터 이동 도구로 충돌 이동
//   (FPhysics2DWorld::MoveMover — CollideMover/SolvePlanes/CastMover 반복, 샘플 sample_character.cpp 방식) → 바닥 판정/붙이기 → EndMove(착지).
//   엔티티 트랜스폼 위치 = 캡슐 중심 (세로 캡슐, 로컬 +Z). 회전·깊이 Y는 바꾸지 않는다 — 몸 방향(좌우 반전)은 이동기가 정하지 않으므로
//   스크립트가 GetMovementVelocity의 X 부호 등으로 스프라이트 FlipX(entity:SetSpriteFlip)를 정한다.
//   모양은 캡슐만 (Box2D 이동 도구가 캡슐 전용이고, 상자는 타일 이음매에 걸린다). 계단 높이는 따로 없다 — 캡슐 아래 반원이
//   반지름의 약 절반 높이 턱은 미끄러지듯 넘는다.
//   2D 콜라이더·RigidBody2D는 쓰지 않는다 (붙이면 별도 바디가 생기므로 붙이지 않는다). 대신 2D 월드에 같은 캡슐의 키네마틱 "대리" 바디를
//   두어 다른 바디가 부딪히고(동적 바디는 밀려남), 레이캐스트·트리거·충돌 알림(OnTriggerEnter 등)이 이 엔티티로 온다.
//   캐릭터끼리 (CharacterCollision): 기본 Ignore = 서로 통과 (다른 캐릭터의 대리 바디를 무시). 두 캐릭터가 모두 Ignore가 아니어야 상호작용하며,
//   움직이는 쪽 설정이 방식을 정한다 — Block: 상대 캡슐(대리 바디)이 벽·바닥 (위에 서고 상대 대리 속도를 발판처럼 물려받는다),
//   Push: Block + 옆으로 막히면 막힌 만큼 × PushStrength 상대를 수평으로 민다 (상대 이동 질의로 벽 안으로는 밀지 않고, 남은 만큼 다시 움직인다 — 한 번).
//     연쇄: 밀린 캐릭터가 그 방향 앞의 다른 상호작용 캐릭터(Block/Push)에 막히면 남은 거리를 그대로 넘겨 민다 (밀린 쪽 설정과 무관 — 줄지어 선
//     캐릭터는 한 덩어리, 밀리는 캐릭터 최대 4, 이미 사슬에 있는 캐릭터 제외, 단계마다 접촉 순서의 첫 캐릭터 하나 — 결정적). 세기는 처음 미는 쪽만.
//     밀림 저항(PushResistance r ≥ 0, 기본 0 = 이전 동작): 밀리는 캐릭터는 넘겨받은 거리 ÷ (1 + r)만 받는다 — 단계마다 받는 쪽 저항으로 나누므로
//     사슬 끝 캐릭터의 이동 = 처음 넘긴 거리 ÷ Π(1 + r_i) (무거운 캐릭터·무거운 줄일수록 덜 밀리고 미는 쪽도 그만큼 덜 나아간다).
//   밟기: Block/Push 캐릭터가 다른 캐릭터 위에 착지하면 이벤트(Stomped — Lua OnStomped(other), 밟힌 쪽 OnStompedBy(other)).
//   상호작용하는 캐릭터는 무브 끝에 대리 바디를 그 자리로 옮겨(이후 같은 틱 다른 캐릭터 무브가 지금 위치를 본다) 처리 순서 = 엔티티 순서.
//   멀티플레이: 예측 클라이언트에서 다른 캐릭터는 스냅샷 보간(과거) 위치에 있으므로 막힘/밟기는 그 위치 기준이고, 서버 결과와 다르면 재조정이
//   맞춘다(보정 허용). 서버에서 밀린 캐릭터의 소유 클라이언트도 ack 재조정으로 밀린 위치를 받는다 (밀기는 무브 밖 서버 일)
struct FCharacterMovement2DComponent
{
	// 다른 2D 캐릭터와의 충돌 (씬 JSON 번호 — 끝에만 추가)
	enum class ECharacterCollision : int32
	{
		Ignore = 0, // 통과 (기존 동작)
		Block  = 1, // 상대가 막는다 (벽·바닥 — 위에 설 수 있다)
		Push   = 2, // 막히면 상대를 민다
	};

	// 이동 방식 (씬 JSON에는 번호로 저장된다 — 끝에만 추가, 인스펙터는 이름 콤보)
	enum class EMode : int32
	{
		Platformer = 0, // 중력·점프·원웨이·경사. 입력은 X만 쓴다
		TopDown    = 1, // 중력 0, 입력 X·Z(8방향, 길이 1로 자름). 점프·내려가기 없음, 항상 "바닥"
	};

	EMode Mode = EMode::Platformer;
	float CapsuleRadius = 30.0f;  // cm
	float CapsuleHeight = 120.0f; // cm, 전체 높이 (반원 포함, 2 × 반지름보다 작으면 원)

	float MaxSpeed           = 600.0f;  // cm/s (수평/탑다운 최대 속력)
	float GroundAcceleration = 6000.0f; // cm/s² 입력 쪽으로 (탑다운은 이 값과 GroundDeceleration만 쓴다)
	float GroundDeceleration = 8000.0f; // cm/s² 입력 없음/반대 방향
	float AirAcceleration    = 4000.0f;
	float AirDeceleration    = 2000.0f;

	float JumpVelocity   = 1100.0f; // cm/s, 점프 순간 위쪽 속도 (공중 점프도 같은 값)
	int32 MaxJumps       = 2;       // 1 = 보통, 2 = 2단 점프 … (바닥에서 걸어 떨어진 뒤 코요테 시간이 지나면 첫 점프를 쓴 것으로 친다)
	float GravityScale   = 3.0f;    // 프로젝트 설정 2D 중력(Gravity2D의 Z) × 배율
	float MaxFallSpeed   = 2000.0f; // cm/s
	float CoyoteTime     = 0.1f;    // 초, 바닥을 떠난 뒤에도 바닥 점프를 허용
	float JumpBufferTime = 0.1f;    // 초, 착지 전에 누른 점프를 기억
	float JumpCutFactor  = 0.5f;    // 가변 점프: 점프 중 상승할 때 StopJumping이면 상승 속도 × 이 값 (1 = 가변 점프 없음)
	float MaxSlopeAngle  = 50.0f;   // 도, 이보다 가파른 면은 바닥이 아니다 (미끄러짐 — 오르려는 수평 속도는 없앤다)
	float GroundSnapDistance = 15.0f; // cm, 걷는 중 아래로 붙이는 거리 (내리막·턱 내려가기)

	float DashSpeed    = 1800.0f; // cm/s
	float DashTime     = 0.15f;   // 초
	float DashCooldown = 0.35f;   // 초 (대시 시작부터)
	int32 MaxAirDashes = 1;       // 공중 대시 횟수 (착지하면 다시 채운다, 바닥 대시는 무제한)
	bool  bDashIgnoresGravity = true; // 대시 중 중력 무시 (끄면 대시 방향 속도에 중력이 더해진다)
	float DropThroughTime = 0.25f; // 초, DropDown 뒤 원웨이 발판을 무시하는 시간

	std::string Layer;              // 충돌 레이어 이름 (이동 질의·대리 바디, 비면 Default — Core/Settings/CollisionSettings.h)
	bool        bClientPrediction = true; // 멀티플레이: 소유 클라이언트가 입력 즉시 미리 움직인다 (프로젝트 설정 네트워크 → 클라이언트 예측도)
	ECharacterCollision CharacterCollision = ECharacterCollision::Ignore; // 다른 2D 캐릭터 (위 주석)
	float               PushStrength       = 1.0f; // Push: 막힌 거리 중 상대에게 넘기는 비율 0~1 (작을수록 무겁게 밀린다, 1 = 이전 동작, 0 = Block과 같음)
	float               PushResistance     = 0.0f; // 밀릴 때 저항 ≥ 0: 넘겨받은 거리 ÷ (1 + 이 값) (0 = 이전 동작, 1 = 절반만 밀림 — 위 주석 연쇄)
};
using ECharacterMovement2DMode = FCharacterMovement2DComponent::EMode;

// 무브 하나 = 한 프레임 입력 (네트워크로 보내고 다시 적용하는 단위)
struct FCharacterMove2D
{
	uint32   Sequence     = 0;    // 소유 클라이언트가 붙이는 순번 (재조정 기준)
	float    DeltaSeconds = 0.0f; // 0~MaxMoveDeltaSeconds로 잘라 적용
	FVector2 Input;               // 평면 이동 방향 (X = 월드 X, Y = 월드 Z, 길이 ≤ 1). 플랫포머는 X만
	FVector2 DashDirection;       // bDash일 때 (0이면 입력 → 속도 X 부호 → +X)
	bool     bJumpPressed = false; // 이번 프레임 Jump() (점프 버퍼에 들어간다)
	bool     bJumpHeld    = false; // 점프 버튼을 아직 누르고 있음 (Jump() ~ StopJumping()) — 가변 점프
	bool     bDash        = false;
	bool     bDropDown    = false;

	static constexpr float MaxMoveDeltaSeconds = 0.1f; // 서버가 받은 dt 상한
	static constexpr float MaxDashSpeed        = 1.0e5f;
};

// 캐릭터 상태 (서버 → 소유 클라이언트 보정, 재조정 시작점). 무브 결과는 이 값 + 월드로만 정해진다
struct FCharacterState2D
{
	FVector2 Position; // 평면 cm (캡슐 중심)
	FVector2 Velocity; // cm/s
	bool     bGrounded         = false;
	bool     bJumpCutAvailable = false; // 점프로 상승 중이고 아직 컷하지 않음
	uint8    JumpsUsed         = 0;
	uint8    AirDashesUsed     = 0;
	float    CoyoteTimer       = 0.0f;
	float    JumpBufferTimer   = 0.0f;
	float    DashTimer         = 0.0f; // > 0 = 대시 중
	float    DashCooldownTimer = 0.0f;
	float    DropTimer         = 0.0f; // > 0 = 원웨이 무시
	FVector2 DashDirection;            // 단위 벡터

	bool IsDashing() const { return DashTimer > 0.0f; }
};

// 무브 하나에서 생긴 일 (Lua OnJumped(n)/OnLanded()/OnDashStarted()/OnStomped(other) — 재조정의 다시 적용에서는 내지 않는다)
struct FCharacterMove2DEvents
{
	bool  bJumped      = false;
	int32 JumpIndex    = 0; // 1 = 첫 점프, 2 = 2단 …
	bool  bLanded      = false;
	bool  bDashStarted = false;
	bool   bStomped      = false; // 다른 캐릭터 위에 착지 (Lua OnStomped(other) / 상대 OnStompedBy(this))
	uint64 StompedEntity = 0;     // 밟은 캐릭터 엔티티 (FEntity::ToId)
};

namespace CharacterMovement2DMath
{
	// 입력 정리: 유한하지 않으면 0, 길이 1로 자른다 (탑다운 대각선이 빠르지 않게). 플랫포머는 Y를 버린다
	FVector2 ClampInput(const FVector2& Input, ECharacterMovement2DMode Mode);
	// Current를 Target 쪽으로 최대 MaxDelta만큼
	float    Approach(float Current, float Target, float MaxDelta);
	FVector2 ApproachVector(const FVector2& Current, const FVector2& Target, float MaxDelta);
	// 바닥 면인가 (법선 Y ≥ cos(MaxSlopeAngle))
	float    GetWalkableNormalY(const FCharacterMovement2DComponent& Movement);
	// 점프 높이 (cm) — v² / 2g (테스트·디자인 도우미)
	float    ComputeJumpHeight(const FCharacterMovement2DComponent& Movement, float GravityZ);

	// 무브 앞 단계 (충돌 전, 순수 함수 — 테스트 대상). State의 타이머·점프·대시·속도를 바꾼다. 규칙:
	//   타이머: 대시 쿨다운·내려가기·점프 버퍼는 dt만큼 줄고, 바닥이면 코요테 = CoyoteTime, 아니면 줄어든다.
	//     공중 + 코요테 끝 + 점프 안 씀이면 첫 점프를 쓴 것으로 친다 (JumpsUsed = 1)
	//   대시: bDash + 쿨다운 0 + 대시 중 아님 + (바닥 || 공중 대시 남음)이면 시작 — 속도 = 방향 × DashSpeed, DashTimer = DashTime.
	//     대시 중에는 그 속도 유지(bDashIgnoresGravity가 꺼져 있으면 중력 더함), 끝나는 무브에서 속도 = 방향 × min(MaxSpeed, DashSpeed). 대시 중 점프는 버퍼에만
	//   플랫포머 수평: 목표 = 입력 X × MaxSpeed, 같은 방향으로 더 빨라질 때 가속(바닥/공중), 그 밖(입력 없음·반대·초과) 감속.
	//     반대 방향이면 max(가속, 감속)
	//   점프: 버퍼(이번 무브에 눌렀거나 JumpBufferTime 안)가 있고, (바닥 || 코요테) + 점프 안 씀이면 바닥 점프, 아니면 JumpsUsed < MaxJumps면 공중 점프.
	//     수직 속도 = JumpVelocity (+ GroundVelocity — 움직이는 발판 속도는 수평 전부, 수직은 위로 갈 때만), 가변 점프 가능
	//   가변 점프: 상승 중 + 컷 가능 + 버튼을 뗐으면 수직 속도 × JumpCutFactor (한 번). 하강하면 컷 불가
	//   중력: 바닥(점프 안 함)이면 수직 0, 아니면 GravityZ × GravityScale × dt, 낙하 속도 MaxFallSpeed로 자름
	//   탑다운: 목표 = 입력(길이 ≤ 1) × MaxSpeed, 벡터로 가속/감속 (GroundAcceleration/Deceleration), 중력·점프 없음
	void BeginMove(const FCharacterMovement2DComponent& Movement, FCharacterState2D& State, const FCharacterMove2D& Move, float GravityZ,
	               const FVector2& GroundVelocity, FCharacterMove2DEvents& OutEvents);
	// 무브 끝 단계 (충돌·바닥 판정 뒤): bGroundedNow로 착지(이벤트, 점프·공중 대시 다시 채움, 컷 끝)·수직 속도 0
	void EndMove(const FCharacterMovement2DComponent& Movement, FCharacterState2D& State, bool bGroundedNow, FCharacterMove2DEvents& OutEvents);
} // namespace CharacterMovement2DMath

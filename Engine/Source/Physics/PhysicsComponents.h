#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

// 강체 운동 형식 (직렬화 값 고정)
enum class EPhysicsMotionType : int32
{
	Static    = 0, // 움직이지 않음 (트랜스폼을 바꾸면 순간이동)
	Kinematic = 1, // 트랜스폼이 물리를 끌고 간다 (충돌로 밀리지 않음)
	Dynamic   = 2, // 물리가 트랜스폼을 결정
};

// 강체. 콜라이더 컴포넌트(박스/구/캡슐 중 하나)와 함께 쓴다. 콜라이더만 있고 강체가 없으면 정적 바디.
// 런타임 바디 핸들은 FPhysicsSystem이 엔티티별로 보관하며 컴포넌트에는 데이터만 둔다 (복제/직렬화 안전).
struct FRigidBodyComponent
{
	int32 MotionType        = static_cast<int32>(EPhysicsMotionType::Dynamic); // EPhysicsMotionType
	float Mass              = 0.0f;   // kg (Dynamic만). 0이면 콜라이더 부피 × Density로 자동 계산
	float Density           = 500.0f; // kg/m³ (Mass가 0일 때만). 기본값은 나무 정도 — 100cm 큐브 500kg, 60cm 상자 108kg
	float Friction          = 0.5f;
	float Restitution       = 0.0f; // 반발 (0 = 튀지 않음, 1 = 완전 탄성)
	float LinearDamping     = 0.05f;
	float AngularDamping    = 0.05f;
	float RollingResistance = 0.05f; // 구르기 저항 계수: 무언가에 닿아 있을 때만 회전을 줄인다 (구는 약 계수 × g로 감속). 0 = 없음
	bool  bUseGravity       = true;
	bool  bLockRotation     = false; // 동적 바디가 회전하지 않음 (캐릭터 캡슐 — 넘어지지 않게, 이동만)
	bool  bReportContacts   = false; // 충돌 시작/끝 이벤트를 낸다 (스크립트가 붙은 엔티티는 켜지 않아도 FGameWorld가 보고시킨다)
};

// 콜라이더 공통: bIsTrigger = 트리거(센서) 영역 — 부딪히지 않고 들어옴/나감만 알린다 (Physics/PhysicsSystem.h 충돌 알림 규칙)

// 박스 콜라이더. 크기는 트랜스폼 월드 스케일이 곱해진다 (바디 생성 시)
struct FBoxColliderComponent
{
	FVector3 HalfExtents = FVector3(50.0f, 50.0f, 50.0f); // cm (기본: 100cm 큐브 = 내장 큐브 메시)
	FVector3 Offset;                                       // cm, 엔티티 로컬
	bool     bIsTrigger = false;
};

struct FSphereColliderComponent
{
	float    Radius = 50.0f; // cm (스케일 성분 중 최대값이 곱해진다)
	FVector3 Offset;
	bool     bIsTrigger = false;
};

// 캡슐: 엔티티 로컬 +Z 축 방향 (Jolt 캡슐은 +Y 축이므로 내부에서 회전)
struct FCapsuleColliderComponent
{
	float    Radius     = 30.0f; // cm
	float    HalfHeight = 60.0f; // cm, 원기둥 부분의 절반 (전체 높이 = 2 * (HalfHeight + Radius))
	FVector3 Offset;
	bool     bIsTrigger = false;
};

// ---- 물리 관절 (규칙은 Physics/PhysicsSystem.h "관절" 절). 이 엔티티(바디 필요)를 Target 엔티티의 바디에 잇는다.
// 공통 필드:
//   Target            연결 대상 (무효이거나 바디가 없으면 월드 그 자리에 고정)
//   Anchor            이 엔티티 로컬 기준 연결 지점 (cm, 스케일 적용). 대상 쪽 지점은 관절을 만드는 순간 같은 월드 위치
//   BreakForce        관절이 버티는 힘 (N, 0 = 끊어지지 않음). 넘으면 끊기고 OnJointBreak(other, force) — 플레이를 다시 시작하거나
//                     컴포넌트를 지웠다 다시 달 때까지 끊긴 채로
//   bCollideConnected 이은 두 바디끼리도 부딪힌다 (기본 끔 — 맞닿은 고리/문틀이 떨지 않게)
// 축은 이 엔티티 로컬 방향 (길이 무관). 관절을 만든 순간의 상대 자세가 기준(0도)이다
struct FFixedJointComponent
{
	FEntity  Target;
	FVector3 Anchor;
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

// 경첩: Axis 둘레로만 돈다 (문, 바퀴). 각도 제한·모터 선택
struct FHingeJointComponent
{
	FEntity  Target;
	FVector3 Anchor;
	FVector3 Axis              = FVector3(0.0f, 0.0f, 1.0f);
	bool     bLimit            = false;
	float    MinAngle          = -90.0f; // 도 (-180 ~ 0)
	float    MaxAngle          = 90.0f;  // 도 (0 ~ 180)
	bool     bMotor            = false;
	float    MotorSpeed        = 90.0f;  // 도/초 (+ = FQuat::FromAxisAngle(Axis, +각) 방향, 예: +Z 축이면 +X → +Y)
	float    MotorMaxTorque    = 100.0f; // N·m
	float    Friction          = 0.0f;   // 모터가 없을 때 회전을 막는 마찰 토크 (N·m)
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

// 거리: Anchor(이 엔티티)와 TargetAnchor(대상 로컬 — 대상이 없으면 월드 위치) 사이 거리를 [Min, Max]로 묶는다. 스프링을 주면 늘어났다 돌아온다
struct FDistanceJointComponent
{
	FEntity  Target;
	FVector3 Anchor;
	FVector3 TargetAnchor;
	float    MinDistance       = -1.0f; // cm (< 0 = 만들 때의 거리)
	float    MaxDistance       = -1.0f; // cm (< 0 = 만들 때의 거리)
	float    SpringFrequency   = 0.0f;  // Hz (0 = 딱딱한 줄/막대)
	float    SpringDamping     = 0.5f;  // 0 = 계속 출렁, 1 = 임계 감쇠
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = true;
};

// 구 관절: 한 점을 중심으로 자유롭게 돈다. ConeAngle < 180이면 Axis가 원뿔 안에서만 흔들린다 (사슬, 진자, 어깨)
struct FBallJointComponent
{
	FEntity  Target;
	FVector3 Anchor;
	FVector3 Axis              = FVector3(0.0f, 0.0f, -1.0f);
	float    ConeAngle         = 180.0f; // 도, 원뿔 반각 (180 = 제한 없음)
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

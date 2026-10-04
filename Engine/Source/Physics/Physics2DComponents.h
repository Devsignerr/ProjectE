#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>

// 2D 물리 컴포넌트 (Box2D, 규칙은 Physics/Physics2DSystem.h 머리 주석).
// 2D 평면 = 월드 X(오른쪽)·Z(위), 깊이 = 월드 Y (바디는 깊이를 바꾸지 않는다). 치수는 cm, 각도는 도(화면 반시계 +).
// 콜라이더만 있고 FRigidBody2DComponent가 없으면 정적 바디. 한 엔티티의 2D 콜라이더(종류마다 하나)는 모두 한 바디의 모양이 된다.

// 2D 바디 운동 형식 (씬 JSON에는 번호로 저장된다 — 끝에만 추가. 인스펙터는 이름 콤보)
enum class EBodyType2D : int32
{
	Static    = 0, // 움직이지 않음 (트랜스폼을 바꾸면 순간이동)
	Kinematic = 1, // 트랜스폼을 따라간다 (충돌로 밀리지 않음)
	Dynamic   = 2, // 물리가 트랜스폼을 결정
};

struct FRigidBody2DComponent
{
	EBodyType2D BodyType       = EBodyType2D::Dynamic;
	float       Mass           = 0.0f; // kg (Dynamic만). 0이면 모양 넓이 × 콜라이더 Density
	float       GravityScale   = 1.0f;
	float       LinearDamping  = 0.0f;
	float       AngularDamping = 0.05f;
	bool        bFixedRotation = false; // 회전하지 않음 (2D 캐릭터)
	bool        bBullet        = false; // 빠른 물체: 동적 바디끼리도 연속 충돌 검사 (비싸다 — 총알 등에만)
	bool        bReportContacts = false; // 충돌 시작/끝 이벤트 (스크립트가 붙은 엔티티는 꺼져 있어도 FGameWorld가 보고시킨다)
	bool        bEnabled       = true;  // 끄면 바디를 만들지 않는다 (충돌·질의 모두 없음)
};

// ---- 콜라이더 공통 필드 (모양마다 반복 — 리플렉션은 평평한 구조체만 다룬다)
//   Offset       엔티티 로컬 평면 오프셋 (cm, X = 로컬 X, Y = 로컬 Z). 엔티티 스케일 X/Z가 곱해진다
//   Friction     마찰 (0~), Restitution 반발 (0 = 튀지 않음), Density 밀도 (kg/m², 강체 Mass가 0일 때 질량 계산)
//   bIsTrigger   트리거: 부딪히지 않고 OnTriggerEnter/Exit만 (정적 바디와는 알리지 않는다 — 3D와 같음)
//   bOneWay      원웨이 플랫폼: 엔티티 로컬 +Z(위) 쪽에서 내려오는 것만 막는다 (아래·옆에서 오는 것은 통과)
//   Layer        충돌 레이어 이름 (프로젝트 설정 "충돌 레이어" — 3D와 같은 행렬). 비었거나 없는 이름 = Default

// 상자: Size = 전체 크기 (cm, X = 폭, Y = 높이), Angle = 엔티티 기준 추가 회전 (도, 반시계 +)
struct FBoxCollider2DComponent
{
	FVector2    Size        = FVector2(100.0f, 100.0f);
	float       Angle       = 0.0f;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 원: 반지름은 스케일 X/Z 중 큰 값이 곱해진다
struct FCircleCollider2DComponent
{
	float       Radius      = 50.0f;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 세로 캡슐 (엔티티 로컬 +Z 축): Height = 전체 높이 (반원 포함, 2 × Radius보다 작으면 원), 반지름은 스케일 X, 높이는 스케일 Z
struct FCapsuleCollider2DComponent
{
	float       Height      = 200.0f;
	float       Radius      = 50.0f;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 볼록 다각형: Points = "x,z; x,z; ..." (cm, 엔티티 로컬 — Physics2DMath::ParsePoints). 3~8점.
// 오목하거나 8점을 넘으면 볼록 껍질(넘으면 넓이를 가장 적게 잃는 점부터 빼서 8점)로 만들고 경고한다
struct FPolygonCollider2DComponent
{
	std::string Points      = "-50,-50; 50,-50; 0,50";
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	float       Density     = 100.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// 선분 체인 (정적 지형·타일 경계용, 질량 없음): Points 형식은 다각형과 같다 (2점 이상). bLoop = 마지막 점과 처음 점도 잇는다.
// 닫힌 체인(4점 이상)은 Box2D 체인(이음매 걸림 없음, 바깥쪽만 막음 — 감는 방향은 자동으로 맞춘다), 그 밖은 양면 선분들
struct FEdgeCollider2DComponent
{
	std::string Points      = "-100,0; 100,0";
	bool        bLoop       = false;
	FVector2    Offset;
	float       Friction    = 0.6f;
	float       Restitution = 0.0f;
	bool        bIsTrigger  = false;
	bool        bOneWay     = false;
	std::string Layer;
};

// ---- 2D 관절 (규칙은 Physics/Physics2DSystem.h "관절" 절 — 3D 관절과 같은 관례). 이 엔티티(2D 바디 필요)를 Target 엔티티의 2D 바디에 잇는다.
// 공통 필드:
//   Target            연결 대상 (무효이거나 2D 바디가 없으면 월드 그 자리에 고정)
//   Anchor            이 엔티티 로컬 평면 기준 연결 지점 (cm, X = 로컬 X, Y = 로컬 Z, 스케일 X/Z 적용). 대상 쪽 지점은 만드는 순간 같은 월드 위치
//   BreakForce        관절이 버티는 힘 (N, 0 = 끊어지지 않음). 넘으면 끊기고 OnJointBreak(other, force) — 플레이를 다시 시작하거나
//                     컴포넌트를 지웠다 다시 달 때까지 끊긴 채로
//   bCollideConnected 이은 두 바디끼리도 부딪힌다
// 각도는 도(화면 반시계 +), 축은 이 엔티티 로컬 평면 방향(길이 무관). 만든 순간의 상대 자세가 기준(0도·이동 0)이다
struct FDistanceJoint2DComponent
{
	FEntity  Target;
	FVector2 Anchor;
	FVector2 TargetAnchor;                 // 대상 로컬 (대상이 없으면 월드 평면 위치)
	float    Length            = -1.0f;    // cm (< 0 = 만들 때의 거리)
	float    MinLength         = -1.0f;    // cm, 스프링일 때 늘어나는 한계 (< 0 = 제한 없음)
	float    MaxLength         = -1.0f;    // cm (< 0 = 제한 없음)
	float    SpringFrequency   = 0.0f;     // Hz (0 = 딱딱한 막대)
	float    SpringDamping     = 0.5f;
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = true;
};

// 회전 관절 (경첩): Anchor를 중심으로 돈다. 각 한계·모터 선택
struct FRevoluteJoint2DComponent
{
	FEntity  Target;
	FVector2 Anchor;
	bool     bLimit            = false;
	float    LowerAngle        = -45.0f; // 도
	float    UpperAngle        = 45.0f;
	bool     bMotor            = false;
	float    MotorSpeed        = 90.0f;  // 도/초 (반시계 +)
	float    MaxMotorTorque    = 100.0f; // N·m
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

// 미닫이 관절: Axis 방향으로만 미끄러진다 (회전 없음). 이동 한계·모터 선택
struct FPrismaticJoint2DComponent
{
	FEntity  Target;
	FVector2 Anchor;
	FVector2 Axis              = FVector2(1.0f, 0.0f);
	bool     bLimit            = false;
	float    LowerTranslation  = -100.0f; // cm
	float    UpperTranslation  = 100.0f;
	bool     bMotor            = false;
	float    MotorSpeed        = 100.0f;  // cm/s (+ = Axis 방향)
	float    MaxMotorForce     = 1000.0f; // N
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

// 용접: 상대 자세를 고정한다. 진동수 > 0이면 부드럽게 (휘는 판자)
struct FWeldJoint2DComponent
{
	FEntity  Target;
	FVector2 Anchor;
	float    LinearFrequency   = 0.0f; // Hz (0 = 딱딱함)
	float    AngularFrequency  = 0.0f; // Hz (0 = 딱딱함)
	float    Damping           = 0.7f;
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

// 바퀴: 이 엔티티(바퀴)가 Target(차체)의 Axis(이 엔티티 로컬 — 기본 위) 방향 서스펜션 위에서 자유롭게 돈다. 모터 = 바퀴 회전
struct FWheelJoint2DComponent
{
	FEntity  Target;
	FVector2 Anchor;
	FVector2 Axis              = FVector2(0.0f, 1.0f);
	float    SpringFrequency   = 4.0f;  // Hz (0 = 서스펜션 없음 — 딱딱함)
	float    SpringDamping     = 0.7f;
	bool     bLimit            = false;
	float    LowerTranslation  = -25.0f; // cm (서스펜션 이동)
	float    UpperTranslation  = 25.0f;
	bool     bMotor            = false;
	float    MotorSpeed        = -360.0f; // 도/초 (반시계 + — 오른쪽으로 굴러가려면 음수)
	float    MaxMotorTorque    = 100.0f;  // N·m
	float    BreakForce        = 0.0f;
	bool     bCollideConnected = false;
};

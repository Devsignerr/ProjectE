#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"
#include "Core/Math/Math.h"
#include "Physics/PhysicsComponents.h"

#include <memory>
#include <vector>

E_DECLARE_ENGINE_LOG_CATEGORY(LogPhysics)

namespace JPH
{
	class BodyInterface;
}

enum class EPhysicsShape : uint8
{
	Box,
	Sphere,
	Capsule,
};

// 바디 생성 정보 (엔진 단위: cm, kg). 모양 치수는 스케일이 이미 곱해진 값
struct FPhysicsBodyDesc
{
	EPhysicsMotionType MotionType = EPhysicsMotionType::Dynamic;
	FVector3           Position;
	FQuat              Rotation;

	EPhysicsShape Shape       = EPhysicsShape::Box;
	FVector3      HalfExtents = FVector3(50.0f, 50.0f, 50.0f);
	float         Radius      = 50.0f;
	float         HalfHeight  = 50.0f;
	FVector3      Offset;       // 바디 로컬 오프셋 (회전 적용 전)

	float  Mass              = 0.0f;   // 0이면 부피 × Density
	float  Density           = 500.0f; // kg/m³
	float  Friction          = 0.5f;
	float  Restitution       = 0.0f;
	float  LinearDamping     = 0.05f;
	float  AngularDamping    = 0.05f;
	float  RollingResistance = 0.05f; // 동적 바디만. 접촉 중 회전 감속
	bool   bUseGravity       = true;
	bool   bLockRotation     = false; // 동적 바디: 이동만 (회전 자유도 없음)
	uint64 UserData          = 0; // 엔티티 ToId()
	// 트리거(센서): 부딪히지 않고 접촉만 알린다. 운동 형식과 무관하게 잠들지 않는 키네마틱 센서로 만든다
	// (정적 센서는 상대가 잠들면 접촉을 잃는다 — Jolt). 트리거 레이어는 정적 바디와 겹침을 계산하지 않는다
	bool   bIsTrigger        = false;
	bool   bReportContacts   = false; // 접촉 시작/끝 이벤트 (트리거는 항상)
};

enum class EPhysicsContactEventType : uint8
{
	Begin,
	End,
};

// 바디 쌍의 접촉 시작/끝 (GetContactEvents). Body1/2는 바디 핸들 순서(작은 쪽이 1), 한쪽 이상이 보고 대상일 때만 생긴다
struct FPhysicsContactEvent
{
	EPhysicsContactEventType Type = EPhysicsContactEventType::Begin;
	uint32   Body1 = ~0u, Body2 = ~0u;
	uint64   UserData1 = 0, UserData2 = 0;
	bool     bSensor = false; // 한쪽이 트리거
	// Begin만 (트리거 제외): 접촉 지점 (cm), 법선 = 바디 2를 1에서 밀어내는 방향, 다가오던 속력 (cm/s, 법선 방향),
	// 충격 세기 추정 (kg·cm/s = 다가오던 속력 × 유효 질량. 접촉이 처음 잡힌 순간 = 솔버 전 값이라 실제 충격량과 다를 수 있다)
	FVector3 Position;
	FVector3 Normal;
	float    ApproachSpeed = 0.0f;
	float    Impulse       = 0.0f;
};

// 캐릭터 (Jolt CharacterVirtual + 다른 물체가 부딪히는 내부 키네마틱 캡슐). 위치 = 캡슐 중심, Z축 캡슐
struct FPhysicsCharacterDesc
{
	FVector3 Position;
	FQuat    Rotation;
	float    Radius          = 35.0f; // cm
	float    HalfHeight      = 55.0f; // cm, 원기둥 절반
	float    MaxSlopeDegrees = 50.0f;
	float    Mass            = 80.0f;   // kg
	float    MaxStrength     = 4000.0f; // N, 동적 물체를 미는 최대 힘
	uint64   UserData        = 0;       // 엔티티 ToId() (내부 바디 — 레이캐스트 결과)
};

enum class EPhysicsConstraintType : uint8
{
	Fixed,
	Hinge,
	Distance,
	Cone,       // 구 관절 (원뿔 제한, 비틀림 자유)
	SwingTwist, // 래그돌 관절 (원뿔 + 비틀림 제한)
};

// 관절 생성 정보 (월드 기준, cm / 라디안). Body1 = 대상(InvalidBody = 월드), Body2 = 이 엔티티 쪽. 한쪽 이상이 동적 바디여야 한다.
// 기준(0도)은 생성 순간의 상대 자세
struct FPhysicsConstraintDesc
{
	EPhysicsConstraintType Type  = EPhysicsConstraintType::Fixed;
	uint32                 Body1 = ~0u;
	uint32                 Body2 = ~0u;
	FVector3               Point1;                          // 연결 지점 (Distance는 바디 1 쪽 점)
	FVector3               Point2;                          // Distance: 바디 2 쪽 점 (나머지는 Point1을 쓴다)
	FVector3               Axis = FVector3(1.0f, 0.0f, 0.0f); // Hinge 회전축 / Cone·SwingTwist 비틀림 축
	FVector3               NormalAxis;                      // Axis에 수직 (0이면 자동) — SwingTwist 평면 축
	bool                   bLimit   = false;                // Hinge 각도 제한
	float                  MinAngle = 0.0f, MaxAngle = 0.0f;
	bool                   bMotor         = false;          // Hinge 속도 모터
	float                  MotorSpeed     = 0.0f;           // rad/s
	float                  MotorMaxTorque = 0.0f;           // N·m
	float                  FrictionTorque = 0.0f;           // N·m (Hinge, SwingTwist)
	float                  MinDistance = -1.0f, MaxDistance = -1.0f; // cm (< 0 = 생성 시 거리)
	float                  SpringFrequency = 0.0f, SpringDamping = 0.0f;
	float                  ConeHalfAngle = 0.0f;                  // Cone/SwingTwist
	float                  TwistMin = 0.0f, TwistMax = 0.0f;       // SwingTwist
	bool                   bCollideConnected = false; // 끄면 두 바디 충돌을 막는다 (DisableCollision)
};

struct FPhysicsCharacterResult
{
	FVector3 Position; // cm
	FVector3 Velocity; // cm/s
	bool     bGrounded = false; // 걸을 수 있는 바닥 위
};

struct FPhysicsRayHit
{
	uint64   UserData = 0;
	FVector3 Position;   // cm
	FVector3 Normal;
	float    Distance = 0.0f; // cm
};

// Jolt PhysicsSystem 래퍼. 엔진 단위(cm)로 입출력하고 내부에서 m로 변환한다.
// 여러 인스턴스를 만들 수 있다 (Jolt 전역 초기화는 참조 카운트).
class FPhysicsWorld
{
public:
	static constexpr uint32 InvalidBody = ~0u;

	FPhysicsWorld();
	~FPhysicsWorld();

	FPhysicsWorld(const FPhysicsWorld&)            = delete;
	FPhysicsWorld& operator=(const FPhysicsWorld&) = delete;

	uint32 CreateBody(const FPhysicsBodyDesc& Desc); // 실패 시 InvalidBody
	void   DestroyBody(uint32 Body);
	uint32 GetBodyCount() const;

	// 순간이동 (정적/동적). 동적 바디의 속도는 유지
	void SetTransform(uint32 Body, const FVector3& Position, const FQuat& Rotation);
	// 키네마틱: 다음 스텝 동안 목표까지 이동하는 속도 설정
	void MoveKinematic(uint32 Body, const FVector3& Position, const FQuat& Rotation, float DeltaSeconds);
	bool GetTransform(uint32 Body, FVector3& OutPosition, FQuat& OutRotation) const;

	// 힘 kg·cm/s², 충격량 kg·cm/s, 속도 cm/s
	void     AddForce(uint32 Body, const FVector3& Force);
	void     AddImpulse(uint32 Body, const FVector3& Impulse);
	void     SetLinearVelocity(uint32 Body, const FVector3& Velocity);
	FVector3 GetLinearVelocity(uint32 Body) const;
	void     SetAngularVelocity(uint32 Body, const FVector3& RadiansPerSecond); // 각속도는 rad/s (축은 엔진 축 그대로)
	FVector3 GetAngularVelocity(uint32 Body) const;
	float    GetMass(uint32 Body) const; // kg, 정적/키네마틱은 0

	// 한 스텝 진행 후 접촉 중인 동적 바디에 구르기 저항을 적용한다
	void Step(float DeltaSeconds);

	// Direction은 정규화하지 않아도 된다. MaxDistance cm
	bool Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsRayHit& OutHit) const;

	// cm/s² (기본 -Z 980.665)
	void SetGravity(const FVector3& Gravity);
	FVector3 GetGravity() const;

	// ---- 캐릭터 (cm). 실패 시 InvalidBody
	uint32 CreateCharacter(const FPhysicsCharacterDesc& Desc);
	void   DestroyCharacter(uint32 Character);
	uint32 GetCharacterCount() const;
	// 속도를 정한 뒤 한 번 이동: 미끄러짐/계단 오르기(StepUp cm)/바닥 붙기(StickDown cm, 0이면 끔 — 점프 직후 등)
	void UpdateCharacter(uint32 Character, float DeltaSeconds, const FVector3& Velocity, float StepUp, float StickDown);
	// 순간이동/보정: 위치·속도를 바꾸고 접촉(바닥 상태)을 다시 계산
	void SetCharacterState(uint32 Character, const FVector3& Position, const FVector3& Velocity);
	void SetCharacterRotation(uint32 Character, const FQuat& Rotation);
	FPhysicsCharacterResult GetCharacterResult(uint32 Character) const;
	// 실제로 닿은 접촉 상대 바디의 UserData (지난 UpdateCharacter/SetCharacterState 기준, 다른 캐릭터 제외)
	void GetCharacterContacts(uint32 Character, std::vector<uint64>& OutUserData) const;
	// 동적 바디와 겹친 가장 깊은 거리 (cm, 0 = 겹침 없음). 지난 UpdateCharacter/SetCharacterState 기준
	float GetCharacterDynamicPenetration(uint32 Character) const;
	// 캐릭터가 동적 바디를 미는(충격량) 여부 — 모든 캐릭터 공용. 끄면 동적 바디도 밀리지 않는 벽처럼 막는다 (예측 재조정의 다시 적용)
	void SetCharactersPushBodies(bool bPush);

	// ---- 접촉 알림. 콜백(Jolt 작업 스레드)에서 모은 것을 Step 끝에 메인 스레드에서 바디 쌍 단위로 정리한다:
	//   같은 쌍의 시작은 한 번, 끝은 실제로 떨어졌을 때 (둘 다 잠들어 생긴 Jolt 제거 통지는 무시 — 계속 닿아 있음),
	//   DestroyBody/DestroyCharacter는 그 바디가 닿아 있던 쌍의 끝을 바로 만든다.
	// 이벤트는 ConsumeContactEvents까지 쌓인다 (Step 여러 번 + 바디 제거)
	void SetBodyReportsContacts(uint32 Body, bool bReport);
	void ConsumeContactEvents(std::vector<FPhysicsContactEvent>& OutEvents); // OutEvents 끝에 붙이고 비운다
	uint32 GetCharacterInnerBody(uint32 Character) const; // 다른 물체가 부딪히는 내부 키네마틱 바디 (없으면 InvalidBody)

	// ---- 관절. 실패(동적 바디 없음 등) 시 InvalidBody. 바디를 지우면 그 바디의 관절도 함께 사라진다 (IsConstraintAlive로 확인)
	uint32 CreateConstraint(const FPhysicsConstraintDesc& Desc);
	void   DestroyConstraint(uint32 Constraint);
	bool   IsConstraintAlive(uint32 Constraint) const;
	uint32 GetConstraintCount() const;
	// 지난 스텝에서 관절이 위치를 지키려고 쓴 힘 (N = 위치 구속 충격량 / 스텝 시간). 끊어짐 판정
	float  GetConstraintForce(uint32 Constraint, float StepSeconds) const;
	// 두 바디 충돌 끄기/켜기 (참조 횟수 — 여러 관절이 같은 쌍을 꺼도 된다). 바디를 지우면 그 바디 항목은 사라진다
	void   DisableCollision(uint32 BodyA, uint32 BodyB);
	void   EnableCollision(uint32 BodyA, uint32 BodyB);
	// ---- 확장: 새 파일이 Jolt 바디를 직접 만들 때 (지형 높이맵 충돌 — TerrainCollision.cpp). 바디 ID = GetIndexAndSequenceNumber (DestroyBody로 지운다)
	JPH::BodyInterface& GetJoltBodyInterface();

private:
	struct FImpl;
	std::unique_ptr<FImpl> Impl;
};

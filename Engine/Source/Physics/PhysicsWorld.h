#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"
#include "Core/Math/Math.h"
#include "Physics/PhysicsComponents.h"

#include <memory>
#include <vector>

E_DECLARE_ENGINE_LOG_CATEGORY(LogPhysics)

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
	// 캐릭터가 동적 바디를 미는(충격량) 여부 — 모든 캐릭터 공용. 끄면 동적 바디도 밀리지 않는 벽처럼 막는다 (예측 재조정의 다시 적용)
	void SetCharactersPushBodies(bool bPush);

private:
	struct FImpl;
	std::unique_ptr<FImpl> Impl;
};

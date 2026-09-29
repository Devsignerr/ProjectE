#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"
#include "Core/Math/Math.h"
#include "Physics/PhysicsComponents.h"

#include <memory>

E_DECLARE_LOG_CATEGORY(LogPhysics)

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

	float  Mass           = 1.0f;
	float  Friction       = 0.5f;
	float  Restitution    = 0.0f;
	float  LinearDamping  = 0.05f;
	float  AngularDamping = 0.05f;
	bool   bUseGravity    = true;
	uint64 UserData       = 0; // 엔티티 ToId()
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

	void Step(float DeltaSeconds);

	// Direction은 정규화하지 않아도 된다. MaxDistance cm
	bool Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsRayHit& OutHit) const;

	// cm/s² (기본 -Z 980.665)
	void SetGravity(const FVector3& Gravity);

private:
	struct FImpl;
	std::unique_ptr<FImpl> Impl;
};

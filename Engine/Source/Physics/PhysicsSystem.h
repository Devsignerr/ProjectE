#pragma once

#include "Core/ECS/Entity.h"
#include "Physics/PhysicsMath.h"
#include "Physics/PhysicsWorld.h"

#include <memory>
#include <unordered_map>

class FScene;

struct FPhysicsHit
{
	FEntity  Entity;
	FVector3 Position; // cm
	FVector3 Normal;
	float    Distance = 0.0f; // cm
};

// 씬 ↔ 물리 월드 동기화 + 60Hz 고정 스텝 + 렌더 보간.
//   대상: 콜라이더(박스 > 구 > 캡슐 중 첫 번째)가 있는 엔티티. FRigidBodyComponent가 없으면 정적.
//   Update 순서: 스크립트 → FPhysicsSystem::Update → FScene::UpdateTransforms
//     1) 새 엔티티 바디 생성 / 사라진 엔티티·콜라이더 바디 제거 / 모양·운동 형식이 바뀌면 다시 생성
//     2) 정적·키네마틱: 트랜스폼 → 물리 (키네마틱은 스텝마다 MoveKinematic)
//     3) 고정 스텝 진행, 동적 바디의 직전/현재 상태 보관
//     4) 동적: 보간된 결과를 트랜스폼에 쓴다 (부모가 있으면 로컬로 역변환). 스크립트가 트랜스폼을 직접 바꿨으면 순간이동으로 처리
// 편집 모드에서는 쓰지 않는다 (플레이 시작 Begin, 정지 End).
class FPhysicsSystem
{
public:
	FPhysicsSystem();
	~FPhysicsSystem();

	FPhysicsSystem(const FPhysicsSystem&)            = delete;
	FPhysicsSystem& operator=(const FPhysicsSystem&) = delete;

	void Begin();
	void End();
	bool IsActive() const { return World != nullptr; }

	// 반환: 이번 프레임 진행한 고정 스텝 수
	uint32 Update(FScene& Scene, float DeltaSeconds);

	// ---- 게임플레이 API (cm, kg). 바디가 없는 엔티티는 무시 / false
	bool     Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsHit& OutHit) const;
	void     AddForce(FEntity Entity, const FVector3& Force);
	void     AddImpulse(FEntity Entity, const FVector3& Impulse);
	void     SetVelocity(FEntity Entity, const FVector3& Velocity);
	FVector3 GetVelocity(FEntity Entity) const;
	bool     HasBody(FEntity Entity) const { return Bodies.contains(Entity); }

	uint32               GetBodyCount() const { return World ? World->GetBodyCount() : 0; }
	const FFixedStepper& GetStepper() const { return Stepper; }
	FPhysicsWorld*       GetWorld() { return World.get(); }

private:
	struct FBodyState
	{
		uint32             Body = FPhysicsWorld::InvalidBody;
		FPhysicsBodyDesc   CreatedDesc; // 바디 생성에 쓴 설정 (모양/운동 형식/재질이 바뀌면 다시 생성)
		EPhysicsMotionType Motion   = EPhysicsMotionType::Static;
		uint64             LastSeenFrame = 0;

		// 동적: 보간용 직전/현재 스텝 상태 (월드), 마지막으로 트랜스폼에 쓴 로컬 값 (스크립트 순간이동 감지)
		FVector3 PreviousPosition, CurrentPosition;
		FQuat    PreviousRotation, CurrentRotation;
		FVector3 WrittenPosition;
		FQuat    WrittenRotation;
		bool     bWritten = false;
	};

	void SyncBodies(FScene& Scene);
	void WriteDynamicTransforms(FScene& Scene);

	std::unique_ptr<FPhysicsWorld>            World;
	std::unordered_map<FEntity, FBodyState>   Bodies;
	FFixedStepper                             Stepper;
	uint64                                    FrameCounter = 0;
};

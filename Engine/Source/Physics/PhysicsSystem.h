#pragma once

#include "Core/ECS/Entity.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsMath.h"
#include "Physics/PhysicsWorld.h"

#include <functional>
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

	// 동적 바디 트랜스폼을 렌더 보간(직전↔현재 스텝)할지. 끄면 최신 스텝 결과를 그대로 쓴다 (서버: 복제할 값에 보간이 섞이지 않게)
	void SetInterpolation(bool bEnabled) { bInterpolate = bEnabled; }
	bool IsInterpolating() const { return bInterpolate; }

	// 참을 돌려주는 엔티티의 동적 바디를 키네마틱으로 만든다 (네트워크 클라이언트: 서버가 시뮬레이션한 복제 엔티티는
	// 복제된 트랜스폼을 따라가고, 로컬 물체와는 충돌한다). 바꾸면 다음 Update에서 해당 바디를 다시 만든다
	void SetKinematicOverride(std::function<bool(const FScene&, FEntity)> Predicate) { KinematicOverride = std::move(Predicate); }

	// ---- 게임플레이 API (cm, kg). 바디가 없는 엔티티는 무시 / false
	bool     Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsHit& OutHit) const;
	void     AddForce(FEntity Entity, const FVector3& Force);
	void     AddImpulse(FEntity Entity, const FVector3& Impulse);
	void     SetVelocity(FEntity Entity, const FVector3& Velocity);
	FVector3 GetVelocity(FEntity Entity) const;
	float    GetMass(FEntity Entity) const; // 실제 바디 질량 (Mass 0 = 밀도 자동 계산 결과). 동적 바디가 아니면 0
	bool     HasBody(FEntity Entity) const { return Bodies.contains(Entity); }

	// ---- 캐릭터 (FCharacterMovementComponent — 규칙은 CharacterMovement.h). 바디 대신 Jolt CharacterVirtual을 만든다.
	// 누가 언제 SimulateCharacter를 부를지는 FGameWorld가 정한다 (소유자 입력/네트워크 무브/관찰자 따라가기)
	void SyncCharacters(FScene& Scene); // 새/사라진 캐릭터, 설정 변경, 스크립트 순간이동 (Update도 부른다)
	bool HasCharacter(FEntity Entity) const { return Characters.contains(Entity); }
	void AddMovementInput(FEntity Entity, const FVector3& WorldDirection); // 이번 프레임 입력에 더한다 (XY만, 무브에서 길이 1로 자름)
	void RequestJump(FEntity Entity);
	// 이번 프레임 입력 → 무브 (입력을 비운다). ControlYaw가 있고 bFaceControlYaw면 그 방향, 아니면 이동 방향을 본다
	FCharacterMove  ConsumePendingMove(FEntity Entity, float DeltaSeconds, const float* ControlYaw);
	void            SimulateCharacter(FScene& Scene, FEntity Entity, const FCharacterMove& Move); // 무브 하나 적용 + 트랜스폼 쓰기
	FCharacterState GetCharacterState(FEntity Entity) const;
	void            SetCharacterState(FScene& Scene, FEntity Entity, const FCharacterState& State); // 보정/재조정 시작점 (트랜스폼도)
	void            FollowTransform(FScene& Scene, FEntity Entity); // 시뮬레이션 없이 캡슐을 현재 트랜스폼에 맞춘다 (복제로 움직이는 다른 플레이어)
	bool            IsGrounded(FEntity Entity) const;
	uint32          GetCharacterCount() const { return static_cast<uint32>(Characters.size()); }

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
	void WriteCharacterTransform(FScene& Scene, FEntity Entity);

	struct FCharacterSim
	{
		uint32                      Character = FPhysicsWorld::InvalidBody;
		FCharacterMovementComponent CreatedWith; // 이 설정으로 만들었다 (모양/질량이 바뀌면 다시 생성)
		FVector2                    PendingInput;
		bool                        bPendingJump = false;
		float                       Yaw          = 0.0f; // 몸 방향 (도)
		FVector3                    WrittenPosition;     // 마지막으로 트랜스폼에 쓴 값 (스크립트 순간이동 감지)
		bool                        bWritten      = false;
		uint64                      LastSeenFrame = 0;
	};
	std::unordered_map<FEntity, FCharacterSim> Characters;
	uint64                                     CharacterSyncCounter = 0;

	std::unique_ptr<FPhysicsWorld>            World;
	std::unordered_map<FEntity, FBodyState>   Bodies;
	FFixedStepper                             Stepper;
	uint64                                    FrameCounter = 0;
	bool                                      bInterpolate = true;
	std::function<bool(const FScene&, FEntity)> KinematicOverride;
};

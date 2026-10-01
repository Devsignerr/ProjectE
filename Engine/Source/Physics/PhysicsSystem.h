#pragma once

#include "Core/ECS/Entity.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsMath.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/CollisionEvents.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class FScene;

struct FPhysicsHit
{
	FEntity  Entity;
	FVector3 Position; // cm
	FVector3 Normal;
	float    Distance = 0.0f; // cm
};

// 동적 바디의 최신 스텝 상태 (월드, 렌더 보간 전)
struct FPhysicsBodyMotion
{
	FVector3 Position;        // cm
	FQuat    Rotation;
	FVector3 LinearVelocity;  // cm/s
	FVector3 AngularVelocity; // rad/s
};

// 씬 ↔ 물리 월드 동기화 + 60Hz 고정 스텝 + 렌더 보간.
//   대상: 콜라이더(박스 > 구 > 캡슐 중 첫 번째)가 있는 엔티티. FRigidBodyComponent가 없으면 정적.
//   Update 순서: 스크립트 → FPhysicsSystem::Update → FScene::UpdateTransforms
//     1) 새 엔티티 바디 생성 / 사라진 엔티티·콜라이더 바디 제거 / 모양·운동 형식이 바뀌면 다시 생성
//     2) 정적·키네마틱: 트랜스폼 → 물리 (키네마틱은 스텝마다 MoveKinematic)
//     3) 고정 스텝 진행, 동적 바디의 직전/현재 상태 보관
//     4) 동적: 보간된 결과를 트랜스폼에 쓴다 (부모가 있으면 로컬로 역변환). 스크립트가 트랜스폼을 직접 바꿨으면 순간이동으로 처리
// 편집 모드에서는 쓰지 않는다 (플레이 시작 Begin, 정지 End).
//
// 충돌 알림 (Scene/CollisionEvents.h): 콜라이더 bIsTrigger = 트리거(센서, 부딪히지 않음), 그 밖은 일반 접촉.
//   보고 대상 바디 = 트리거 || 강체 bReportContacts || SetContactReportFilter가 참인 엔티티(FGameWorld: 스크립트가 붙은 엔티티).
//   쌍의 한쪽만 보고 대상이어도 양쪽 엔티티 이벤트가 생긴다. 보고 대상이 아닌 쌍은 Jolt 콜백에서 바로 버린다 (비용).
//   Jolt 콜백(작업 스레드)에서 모은 것을 스텝 뒤 메인 스레드에서 정리해 GetCollisionEvents에 쌓는다 (Update마다 처음에 비움).
//   전달(스크립트/게임 모듈)은 FGameWorld가 물리·UpdateTransforms 뒤에 한다. 캐릭터는 내부 키네마틱 바디로 감지된다
//   (트리거에 들어옴, 동적 물체와 부딪힘 — 정적 벽에 닿는 것은 캐릭터 이동 쪽이라 알리지 않는다)
//   바디를 다시 만들면(모양/운동 형식 변경) 그 쌍은 끝 → 다시 시작으로 보인다
//
// 관절 (FFixed/Hinge/Distance/BallJointComponent, 필드 설명은 PhysicsComponents.h): 바디 동기화 뒤 엔티티의 바디(Body2)와
//   Target의 바디(Body1, 없으면 월드 · 캐릭터면 내부 키네마틱 바디)를 잇는다. 연결 지점/축은 만드는 순간의 바디 자세 기준.
//   설정이나 양쪽 바디가 바뀌면 지금 자세로 다시 만든다. 둘 다 동적이 아니면(예: 클라이언트의 복제 키네마틱 바디끼리) 만들지 않는다.
//   끊어짐: BreakForce > 0이면 스텝마다 위치 구속 힘(N)을 재서 넘으면 지우고 JointBreak 이벤트 (GetCollisionEvents) —
//   컴포넌트를 지우거나 플레이를 다시 시작할 때까지 끊긴 채로 둔다
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

	// ---- 충돌 알림 (클래스 주석). 필터: 이 밖의 엔티티도 보고 대상으로 (매 Sync 다시 묻는다, nullptr = 없음)
	void SetContactReportFilter(std::function<bool(const FScene&, FEntity)> Predicate) { ContactReportFilter = std::move(Predicate); }
	const std::vector<FCollisionEvent>& GetCollisionEvents() const { return CollisionEvents; } // 지난 Update에서 생긴 것

	// ---- 게임플레이 API (cm, kg). 바디가 없는 엔티티는 무시 / false
	bool     Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsHit& OutHit) const;
	void     AddForce(FEntity Entity, const FVector3& Force);
	void     AddImpulse(FEntity Entity, const FVector3& Impulse);
	void     SetVelocity(FEntity Entity, const FVector3& Velocity);
	FVector3 GetVelocity(FEntity Entity) const;
	float    GetMass(FEntity Entity) const; // 실제 바디 질량 (Mass 0 = 밀도 자동 계산 결과). 동적 바디가 아니면 0
	bool     HasBody(FEntity Entity) const { return Bodies.contains(Entity); }
	bool     IsDynamicBody(FEntity Entity) const;

	// ---- 동적 바디 상태 직접 다루기 (네트워크 클라이언트의 물리 예측 — World/GameWorldPhysicsPrediction.cpp). 동적 바디가 아니면 무시 / false
	// 바디 생성/제거/운동 형식 변경을 지금 반영 (Update도 부른다). 키네마틱 오버라이드를 바꾼 직후 바디 상태를 정할 때
	void SyncBodies(FScene& Scene);
	bool GetBodyMotion(FEntity Entity, FPhysicsBodyMotion& OutMotion) const;
	// 순간이동 + 속도 (렌더 보간도 끊는다 — 진입/스냅)
	void SetBodyMotion(FEntity Entity, const FPhysicsBodyMotion& Motion);
	// 작은 보정: 위치/회전/속도에 더한다. 렌더 보간의 직전 상태도 같이 옮겨 화면이 끊기지 않는다
	void CorrectBody(FEntity Entity, const FVector3& DeltaPosition, const FQuat& DeltaRotation, const FVector3& DeltaVelocity, const FVector3& DeltaAngularVelocity);
	// 충돌 질의용으로만 잠시 옮긴다 (보간/스텝 상태는 그대로). RestoreBodyPose로 최신 스텝 위치로 되돌린다 — 예측 재조정에서 캐릭터 무브를 다시 적용할 때
	void PoseBody(FEntity Entity, const FVector3& Position, const FQuat& Rotation);
	void RestoreBodyPose(FEntity Entity);

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
	// 화면용 위치 오프셋 (시뮬레이션 위치는 그대로, 트랜스폼에만 더한다) — 예측 보정을 부드럽게 흡수할 때
	void            SetCharacterVisualOffset(FScene& Scene, FEntity Entity, const FVector3& Offset);
	uint32          GetCharacterCount() const { return static_cast<uint32>(Characters.size()); }
	// 캐릭터가 지난 이동에서 실제로 닿은 바디의 엔티티 (바닥 포함, 다른 캐릭터 제외)
	void            GetCharacterContacts(FEntity Entity, std::vector<FEntity>& OutEntities) const;
	float           GetCharacterDynamicPenetration(FEntity Entity) const; // 동적 바디와 겹친 깊이 (cm)
	// 캐릭터가 동적 바디를 미는지 (모든 캐릭터). 예측 재조정에서 무브를 다시 적용하는 동안 끈다 — 이미 민 물체를 또 밀지 않게
	void            SetCharactersPushBodies(bool bPush);

	// ---- 관절 (클래스 주석)
	uint32 GetJointCount() const;                // 지금 살아 있는 관절 수
	bool   IsJointBroken(FEntity Entity) const;  // 이 엔티티의 관절 컴포넌트 중 하나라도 끊어졌는가
	bool   HasJoint(FEntity Entity) const;       // 이 엔티티의 관절이 하나라도 만들어져 있는가

	// ---- 래그돌 (규칙은 Physics/Ragdoll.h). Entity = 모델 루트(FAnimationComponent) 또는 그 조상 — 자신이 아니면 자손에서 찾는다
	bool   EnableRagdoll(FScene& Scene, FEntity Entity); // 이미 켜져 있거나 뼈대가 없으면 false
	void   DisableRagdoll(FScene& Scene, FEntity Entity);
	bool   IsRagdollActive(const FScene& Scene, FEntity Entity) const;
	uint32 GetRagdollPartCount(const FScene& Scene, FEntity Entity) const; // 켜진 래그돌의 캡슐 수 (0 = 꺼짐)
	static FEntity FindRagdollModel(const FScene& Scene, FEntity Entity);   // 래그돌을 만들 모델 루트 (없으면 무효)

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

	void WriteDynamicTransforms(FScene& Scene);
	void WriteCharacterTransform(FScene& Scene, FEntity Entity);

	struct FCharacterSim
	{
		uint32                      Character = FPhysicsWorld::InvalidBody;
		FCharacterMovementComponent CreatedWith; // 이 설정으로 만들었다 (모양/질량이 바뀌면 다시 생성)
		FVector2                    PendingInput;
		bool                        bPendingJump = false;
		FVector3                    VisualOffset;        // 트랜스폼에만 더하는 화면용 오프셋 (예측 보정 흡수)
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

	// 관절: (엔티티, 종류)마다 하나
	struct FJointKey
	{
		FEntity Entity;
		uint8   Kind = 0;
		bool    operator==(const FJointKey& Other) const { return Entity == Other.Entity && Kind == Other.Kind; }
	};
	struct FJointKeyHash
	{
		size_t operator()(const FJointKey& Key) const noexcept { return std::hash<uint64>{}(Key.Entity.ToId() * 4u + Key.Kind); }
	};
	struct FJointState
	{
		uint32             Constraint = FPhysicsWorld::InvalidBody; // 실패/끊김이면 무효
		uint32             Body1      = FPhysicsWorld::InvalidBody;
		uint32             Body2      = FPhysicsWorld::InvalidBody;
		std::vector<float> Signature; // 만들 때의 설정 (바뀌면 다시 만든다)
		FEntity            Target;
		float              BreakForce = 0.0f;
		bool               bBroken    = false;
		uint64             LastSeenFrame = 0;
	};
	void SyncJoints(FScene& Scene);
	void CheckJointBreaks();
	std::unordered_map<FJointKey, FJointState, FJointKeyHash> Joints;

	// 래그돌 (Physics/PhysicsRagdoll.cpp)
	struct FRagdollPart
	{
		int32      Node = -1;
		uint32     Body = FPhysicsWorld::InvalidBody;
		FMatrix4x4 BoneFromBody;   // 뼈 월드(강체) = BoneFromBody * 캡슐 월드 (행벡터: 뼈가 캡슐의 자식)
		FVector3   BoneScale;      // 켤 때의 뼈 월드 스케일
		FVector3   PreviousPosition, CurrentPosition;
		FQuat      PreviousRotation, CurrentRotation;
	};
	struct FSavedNodePose
	{
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale;
	};
	struct FRagdollState
	{
		std::vector<FRagdollPart>   Parts;
		std::vector<uint32>         Constraints;
		std::vector<int32>          PartOfNode; // 노드 → 캡슐 (-1 = 부모를 따라간다)
		std::vector<int32>          NodeOrder;  // 부모 먼저
		std::vector<FSavedNodePose> Saved;      // 켤 때의 노드 로컬 트랜스폼 (끄면 되돌린다)
	};
	std::unordered_map<FEntity, FRagdollState> Ragdolls; // 모델 루트 → 래그돌
	void DestroyRagdollBodies(FRagdollState& State);
	void SyncRagdolls(FScene& Scene);     // 사라진 모델 정리
	void WriteRagdollPoses(FScene& Scene); // 캡슐 → 뼈 로컬 트랜스폼

	void CollectContactEvents(); // 월드 이벤트 → 엔티티 기준 이벤트 (양쪽)
	std::function<bool(const FScene&, FEntity)> ContactReportFilter;
	std::vector<FCollisionEvent>                CollisionEvents;
	std::vector<FPhysicsContactEvent>           ContactScratch;
};

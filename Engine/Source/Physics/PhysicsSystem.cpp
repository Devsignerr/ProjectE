#include "Physics/PhysicsSystem.h"

#include "Core/Settings/ProjectSettings.h"
#include "Physics/PhysicsComponents.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr float TeleportPositionTolerance = 0.01f;   // cm
	constexpr float TeleportRotationTolerance = 1.0e-5f; // 1 - |dot|

	FMatrix4x4 ComputeWorldMatrix(const FScene& Scene, FEntity Entity)
	{
		// 부모(또는 부착 소켓) 월드 행렬은 직전 UpdateTransforms 기준 (행벡터 규약: 로컬 먼저)
		return Scene.GetTransform(Entity).GetLocalMatrix() * Scene.GetParentWorldMatrix(Entity);
	}

	float MaxAbs(float A, float B) { return std::max(std::abs(A), std::abs(B)); }

	// 콜라이더 → 바디 모양 (스케일 적용). 박스 > 구 > 캡슐 순으로 하나만 사용
	bool FillShape(const FRegistry& Registry, FEntity Entity, const FVector3& Scale, FPhysicsBodyDesc& Desc)
	{
		if (const FBoxColliderComponent* Box = Registry.TryGet<FBoxColliderComponent>(Entity))
		{
			Desc.Shape       = EPhysicsShape::Box;
			Desc.HalfExtents = FVector3(std::abs(Box->HalfExtents.X * Scale.X), std::abs(Box->HalfExtents.Y * Scale.Y), std::abs(Box->HalfExtents.Z * Scale.Z));
			Desc.Offset      = FVector3(Box->Offset.X * Scale.X, Box->Offset.Y * Scale.Y, Box->Offset.Z * Scale.Z);
			return true;
		}
		if (const FSphereColliderComponent* Sphere = Registry.TryGet<FSphereColliderComponent>(Entity))
		{
			Desc.Shape  = EPhysicsShape::Sphere;
			Desc.Radius = std::abs(Sphere->Radius) * MaxAbs(MaxAbs(Scale.X, Scale.Y), Scale.Z);
			Desc.Offset = FVector3(Sphere->Offset.X * Scale.X, Sphere->Offset.Y * Scale.Y, Sphere->Offset.Z * Scale.Z);
			return true;
		}
		if (const FCapsuleColliderComponent* Capsule = Registry.TryGet<FCapsuleColliderComponent>(Entity))
		{
			Desc.Shape      = EPhysicsShape::Capsule;
			Desc.Radius     = std::abs(Capsule->Radius) * MaxAbs(Scale.X, Scale.Y);
			Desc.HalfHeight = std::abs(Capsule->HalfHeight * Scale.Z);
			Desc.Offset     = FVector3(Capsule->Offset.X * Scale.X, Capsule->Offset.Y * Scale.Y, Capsule->Offset.Z * Scale.Z);
			return true;
		}
		return false;
	}

	// 스케일에서 나온 치수는 회전 행렬 분해 오차(수 ULP)로 매 프레임 흔들리므로 상대 오차로 비교한다
	constexpr float ShapeRelativeTolerance = 1.0e-4f;

	bool IsNearlySameDimension(float A, float B)
	{
		return std::abs(A - B) <= ShapeRelativeTolerance * std::max({ 1.0f, std::abs(A), std::abs(B) });
	}

	// 바디를 다시 만들어야 하는지 (위치/회전은 제외). 컴포넌트 값은 정확히, 스케일 파생 치수는 허용 오차로 비교
	bool NeedsRecreate(const FPhysicsBodyDesc& Old, const FPhysicsBodyDesc& New)
	{
		if (Old.MotionType != New.MotionType || Old.Shape != New.Shape || Old.bUseGravity != New.bUseGravity || Old.Mass != New.Mass ||
		    Old.Density != New.Density || Old.Friction != New.Friction || Old.Restitution != New.Restitution ||
		    Old.LinearDamping != New.LinearDamping || Old.AngularDamping != New.AngularDamping || Old.RollingResistance != New.RollingResistance)
		{
			return true;
		}
		const float OldDimensions[] = { Old.HalfExtents.X, Old.HalfExtents.Y, Old.HalfExtents.Z, Old.Radius, Old.HalfHeight, Old.Offset.X, Old.Offset.Y, Old.Offset.Z };
		const float NewDimensions[] = { New.HalfExtents.X, New.HalfExtents.Y, New.HalfExtents.Z, New.Radius, New.HalfHeight, New.Offset.X, New.Offset.Y, New.Offset.Z };
		for (size_t Index = 0; Index < std::size(OldDimensions); ++Index)
		{
			if (!IsNearlySameDimension(OldDimensions[Index], NewDimensions[Index]))
			{
				return true;
			}
		}
		return false;
	}

	bool IsSameRotation(const FQuat& A, const FQuat& B)
	{
		return 1.0f - std::abs(FQuat::Dot(A, B)) <= TeleportRotationTolerance;
	}
} // namespace

FPhysicsSystem::FPhysicsSystem()  = default;
FPhysicsSystem::~FPhysicsSystem() = default;

void FPhysicsSystem::Begin()
{
	End();
	World = std::make_unique<FPhysicsWorld>();
	// 프로젝트 설정 "물리" (플레이를 시작할 때마다 다시 읽는다)
	const FPhysicsSettings& Settings = FProjectSettings::Get().Physics;
	World->SetGravity(Settings.Gravity);
	Stepper.StepSeconds = 1.0f / std::clamp(Settings.FixedStepHz, 15.0f, 240.0f);
	Stepper.MaxSteps    = std::clamp(Settings.MaxSubSteps, 1u, 16u);
	Stepper.Reset();
}

void FPhysicsSystem::End()
{
	Characters.clear(); // 월드가 캐릭터와 함께 사라진다
	Bodies.clear();
	World.reset();
	Stepper.Reset();
}

uint32 FPhysicsSystem::Update(FScene& Scene, float DeltaSeconds)
{
	if (!World)
	{
		return 0;
	}
	++FrameCounter;
	SyncCharacters(Scene);
	SyncBodies(Scene);

	const uint32 Steps = Stepper.Advance(DeltaSeconds);
	for (uint32 Step = 0; Step < Steps; ++Step)
	{
		const float StepAlpha = static_cast<float>(Step + 1) / static_cast<float>(Steps);
		for (auto& [Entity, State] : Bodies)
		{
			if (State.Motion == EPhysicsMotionType::Kinematic)
			{
				// 이번 프레임 목표까지 스텝마다 나눠 이동
				World->MoveKinematic(State.Body, FVector3::Lerp(State.PreviousPosition, State.CurrentPosition, StepAlpha),
				                     FQuat::Slerp(State.PreviousRotation, State.CurrentRotation, StepAlpha), Stepper.StepSeconds);
			}
			else if (State.Motion == EPhysicsMotionType::Dynamic)
			{
				State.PreviousPosition = State.CurrentPosition;
				State.PreviousRotation = State.CurrentRotation;
			}
		}

		World->Step(Stepper.StepSeconds);

		for (auto& [Entity, State] : Bodies)
		{
			if (State.Motion == EPhysicsMotionType::Dynamic)
			{
				World->GetTransform(State.Body, State.CurrentPosition, State.CurrentRotation);
			}
		}
	}
	if (Steps > 0)
	{
		for (auto& [Entity, State] : Bodies)
		{
			if (State.Motion == EPhysicsMotionType::Kinematic)
			{
				State.PreviousPosition = State.CurrentPosition;
				State.PreviousRotation = State.CurrentRotation;
			}
		}
	}

	WriteDynamicTransforms(Scene);
	return Steps;
}

void FPhysicsSystem::SyncBodies(FScene& Scene)
{
	if (!World)
	{
		return;
	}
	FRegistry& Registry = Scene.GetRegistry();

	// 콜라이더를 가진 엔티티 수집 (순회 중에는 컴포넌트를 바꾸지 않지만 바디 생성은 밖에서)
	std::vector<FEntity>        Candidates;
	std::unordered_set<FEntity> Seen;
	auto                        Collect = [&](FEntity Entity) {
        if (Seen.insert(Entity).second)
        {
            Candidates.push_back(Entity);
        }
	};
	Registry.View<FTransformComponent, FBoxColliderComponent>().Each([&](FEntity Entity, FTransformComponent&, FBoxColliderComponent&) { Collect(Entity); });
	Registry.View<FTransformComponent, FSphereColliderComponent>().Each([&](FEntity Entity, FTransformComponent&, FSphereColliderComponent&) { Collect(Entity); });
	Registry.View<FTransformComponent, FCapsuleColliderComponent>().Each([&](FEntity Entity, FTransformComponent&, FCapsuleColliderComponent&) { Collect(Entity); });

	for (FEntity Entity : Candidates)
	{
		if (Registry.Has<FCharacterMovementComponent>(Entity))
		{
			continue; // 캐릭터는 콜라이더가 있어도 CharacterVirtual (SyncCharacters)
		}
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale;
		PhysicsMath::DecomposeWorld(ComputeWorldMatrix(Scene, Entity), Position, Rotation, Scale);

		FPhysicsBodyDesc Desc;
		if (!FillShape(Registry, Entity, Scale, Desc))
		{
			continue;
		}
		Desc.Position = Position;
		Desc.Rotation = Rotation;
		Desc.UserData = Entity.ToId();
		if (const FRigidBodyComponent* RigidBody = Registry.TryGet<FRigidBodyComponent>(Entity))
		{
			Desc.MotionType     = static_cast<EPhysicsMotionType>(std::clamp(RigidBody->MotionType, 0, 2));
			Desc.Mass              = RigidBody->Mass;
			Desc.Density           = RigidBody->Density;
			Desc.Friction          = RigidBody->Friction;
			Desc.Restitution       = RigidBody->Restitution;
			Desc.LinearDamping     = RigidBody->LinearDamping;
			Desc.AngularDamping    = RigidBody->AngularDamping;
			Desc.RollingResistance = RigidBody->RollingResistance;
			Desc.bUseGravity       = RigidBody->bUseGravity;
			Desc.bLockRotation     = RigidBody->bLockRotation;
			if (Desc.MotionType == EPhysicsMotionType::Dynamic && KinematicOverride && KinematicOverride(Scene, Entity))
			{
				Desc.MotionType = EPhysicsMotionType::Kinematic;
			}
		}
		else
		{
			Desc.MotionType = EPhysicsMotionType::Static;
		}
		auto Found = Bodies.find(Entity);
		if (Found == Bodies.end() || NeedsRecreate(Found->second.CreatedDesc, Desc))
		{
			if (Found != Bodies.end())
			{
				World->DestroyBody(Found->second.Body);
			}
			FBodyState State;
			State.Body = World->CreateBody(Desc);
			if (State.Body == FPhysicsWorld::InvalidBody)
			{
				if (Found != Bodies.end())
				{
					Bodies.erase(Found);
				}
				continue;
			}
			State.CreatedDesc      = Desc;
			State.Motion           = Desc.MotionType;
			State.LastSeenFrame    = FrameCounter;
			State.PreviousPosition = State.CurrentPosition = Position;
			State.PreviousRotation = State.CurrentRotation = Rotation;
			Bodies[Entity]         = State;
			continue;
		}

		FBodyState& State   = Found->second;
		State.LastSeenFrame = FrameCounter;
		switch (State.Motion)
		{
		case EPhysicsMotionType::Static:
			if (FVector3::DistanceSquared(State.CurrentPosition, Position) > TeleportPositionTolerance * TeleportPositionTolerance ||
			    !IsSameRotation(State.CurrentRotation, Rotation))
			{
				World->SetTransform(State.Body, Position, Rotation);
				State.CurrentPosition = Position;
				State.CurrentRotation = Rotation;
			}
			break;
		case EPhysicsMotionType::Kinematic:
			State.CurrentPosition = Position; // 스텝 동안 Previous → Current로 이동
			State.CurrentRotation = Rotation;
			break;
		case EPhysicsMotionType::Dynamic:
		{
			// 스크립트/에디터가 트랜스폼을 직접 바꿨으면 순간이동
			const FTransformComponent& Transform = Scene.GetTransform(Entity);
			if (State.bWritten &&
			    (FVector3::DistanceSquared(Transform.Position, State.WrittenPosition) > TeleportPositionTolerance * TeleportPositionTolerance ||
			     !IsSameRotation(Transform.Rotation, State.WrittenRotation)))
			{
				World->SetTransform(State.Body, Position, Rotation);
				State.PreviousPosition = State.CurrentPosition = Position;
				State.PreviousRotation = State.CurrentRotation = Rotation;
			}
			break;
		}
		}
	}

	// 사라진 엔티티/콜라이더
	for (auto It = Bodies.begin(); It != Bodies.end();)
	{
		if (It->second.LastSeenFrame != FrameCounter)
		{
			World->DestroyBody(It->second.Body);
			It = Bodies.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FPhysicsSystem::WriteDynamicTransforms(FScene& Scene)
{
	const float Alpha = bInterpolate ? Stepper.GetAlpha() : 1.0f;
	for (auto& [Entity, State] : Bodies)
	{
		if (State.Motion != EPhysicsMotionType::Dynamic)
		{
			continue;
		}
		const FVector3 WorldPosition = FVector3::Lerp(State.PreviousPosition, State.CurrentPosition, Alpha);
		const FQuat    WorldRotation = FQuat::Slerp(State.PreviousRotation, State.CurrentRotation, Alpha).GetNormalized();

		FTransformComponent& Transform = Scene.GetTransform(Entity);
		if (Scene.GetParent(Entity).IsValid() || Scene.IsSocketAttached(Entity))
		{
			PhysicsMath::WorldToLocal(Scene.GetParentWorldMatrix(Entity), WorldPosition, WorldRotation, Transform.Position, Transform.Rotation);
		}
		else
		{
			Transform.Position = WorldPosition;
			Transform.Rotation = WorldRotation;
		}
		State.WrittenPosition = Transform.Position;
		State.WrittenRotation = Transform.Rotation;
		State.bWritten        = true;
	}
}

bool FPhysicsSystem::Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsHit& OutHit) const
{
	FPhysicsRayHit Hit;
	if (!World || !World->Raycast(Origin, Direction, MaxDistance, Hit))
	{
		return false;
	}
	OutHit.Entity   = FEntity::FromId(Hit.UserData);
	OutHit.Position = Hit.Position;
	OutHit.Normal   = Hit.Normal;
	OutHit.Distance = Hit.Distance;
	return true;
}

void FPhysicsSystem::AddForce(FEntity Entity, const FVector3& Force)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->AddForce(Found->second.Body, Force);
	}
}

void FPhysicsSystem::AddImpulse(FEntity Entity, const FVector3& Impulse)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->AddImpulse(Found->second.Body, Impulse);
	}
}

void FPhysicsSystem::SetVelocity(FEntity Entity, const FVector3& Velocity)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		World->SetLinearVelocity(Found->second.Body, Velocity);
	}
}

FVector3 FPhysicsSystem::GetVelocity(FEntity Entity) const
{
	if (const auto Character = Characters.find(Entity); World && Character != Characters.end())
	{
		return World->GetCharacterResult(Character->second.Character).Velocity;
	}
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		return World->GetLinearVelocity(Found->second.Body);
	}
	return FVector3();
}

bool FPhysicsSystem::IsDynamicBody(FEntity Entity) const
{
	const auto Found = Bodies.find(Entity);
	return World && Found != Bodies.end() && Found->second.Motion == EPhysicsMotionType::Dynamic;
}

bool FPhysicsSystem::GetBodyMotion(FEntity Entity, FPhysicsBodyMotion& OutMotion) const
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.Motion != EPhysicsMotionType::Dynamic)
	{
		return false;
	}
	const FBodyState& State   = Found->second;
	OutMotion.Position        = State.CurrentPosition;
	OutMotion.Rotation        = State.CurrentRotation;
	OutMotion.LinearVelocity  = World->GetLinearVelocity(State.Body);
	OutMotion.AngularVelocity = World->GetAngularVelocity(State.Body);
	return true;
}

void FPhysicsSystem::SetBodyMotion(FEntity Entity, const FPhysicsBodyMotion& Motion)
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.Motion != EPhysicsMotionType::Dynamic)
	{
		return;
	}
	FBodyState&  State    = Found->second;
	const FQuat  Rotation = Motion.Rotation.GetNormalized();
	World->SetTransform(State.Body, Motion.Position, Rotation);
	World->SetLinearVelocity(State.Body, Motion.LinearVelocity);
	World->SetAngularVelocity(State.Body, Motion.AngularVelocity);
	State.PreviousPosition = State.CurrentPosition = Motion.Position;
	State.PreviousRotation = State.CurrentRotation = Rotation;
}

void FPhysicsSystem::CorrectBody(FEntity Entity, const FVector3& DeltaPosition, const FQuat& DeltaRotation, const FVector3& DeltaVelocity,
                                 const FVector3& DeltaAngularVelocity)
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.Motion != EPhysicsMotionType::Dynamic)
	{
		return;
	}
	FBodyState& State      = Found->second;
	State.CurrentPosition  = State.CurrentPosition + DeltaPosition;
	State.PreviousPosition = State.PreviousPosition + DeltaPosition;
	State.CurrentRotation  = (DeltaRotation * State.CurrentRotation).GetNormalized();
	State.PreviousRotation = (DeltaRotation * State.PreviousRotation).GetNormalized();
	World->SetTransform(State.Body, State.CurrentPosition, State.CurrentRotation);
	if (DeltaVelocity.LengthSquared() > 0.0f)
	{
		World->SetLinearVelocity(State.Body, World->GetLinearVelocity(State.Body) + DeltaVelocity);
	}
	if (DeltaAngularVelocity.LengthSquared() > 0.0f)
	{
		World->SetAngularVelocity(State.Body, World->GetAngularVelocity(State.Body) + DeltaAngularVelocity);
	}
}

void FPhysicsSystem::PoseBody(FEntity Entity, const FVector3& Position, const FQuat& Rotation)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end() && Found->second.Motion == EPhysicsMotionType::Dynamic)
	{
		World->SetTransform(Found->second.Body, Position, Rotation.GetNormalized());
	}
}

void FPhysicsSystem::RestoreBodyPose(FEntity Entity)
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end() && Found->second.Motion == EPhysicsMotionType::Dynamic)
	{
		World->SetTransform(Found->second.Body, Found->second.CurrentPosition, Found->second.CurrentRotation);
	}
}

float FPhysicsSystem::GetMass(FEntity Entity) const
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		return World->GetMass(Found->second.Body);
	}
	return 0.0f;
}

// ---------------------------------------------------------------- 캐릭터

namespace
{
	bool NeedsCharacterRecreate(const FCharacterMovementComponent& A, const FCharacterMovementComponent& B)
	{
		return A.CapsuleRadius != B.CapsuleRadius || A.CapsuleHalfHeight != B.CapsuleHalfHeight || A.MaxSlopeAngle != B.MaxSlopeAngle || A.Mass != B.Mass ||
		       A.PushForce != B.PushForce;
	}

	float YawFromDirection(const FVector2& Direction)
	{
		return FMath::RadiansToDegrees(std::atan2(Direction.Y, Direction.X));
	}
} // namespace

void FPhysicsSystem::SyncCharacters(FScene& Scene)
{
	if (!World)
	{
		return;
	}
	++CharacterSyncCounter;
	FRegistry&           Registry = Scene.GetRegistry();
	std::vector<FEntity> Entities;
	Registry.View<FCharacterMovementComponent, FTransformComponent>().Each(
		[&](FEntity Entity, FCharacterMovementComponent&, FTransformComponent&) { Entities.push_back(Entity); });

	for (const FEntity Entity : Entities)
	{
		const FCharacterMovementComponent& Movement = Registry.Get<FCharacterMovementComponent>(Entity);
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale;
		PhysicsMath::DecomposeWorld(ComputeWorldMatrix(Scene, Entity), Position, Rotation, Scale);

		auto Found = Characters.find(Entity);
		if (Found == Characters.end() || NeedsCharacterRecreate(Found->second.CreatedWith, Movement))
		{
			FCharacterSim Sim;
			if (Found != Characters.end())
			{
				World->DestroyCharacter(Found->second.Character);
				Sim = Found->second;
			}
			FPhysicsCharacterDesc Desc;
			Desc.Position        = Position;
			Desc.Rotation        = FQuat::FromEuler(0.0f, Sim.Yaw, 0.0f);
			Desc.Radius          = Movement.CapsuleRadius;
			Desc.HalfHeight      = Movement.CapsuleHalfHeight;
			Desc.MaxSlopeDegrees = Movement.MaxSlopeAngle;
			Desc.Mass            = Movement.Mass;
			Desc.MaxStrength     = Movement.PushForce;
			Desc.UserData        = Entity.ToId();
			if (Found == Characters.end())
			{
				// 처음: 몸 방향 = 트랜스폼의 yaw (PlayerStart 방향 등)
				const FVector3 Forward = Rotation.GetForwardVector();
				Sim.Yaw                = YawFromDirection(FVector2(Forward.X, Forward.Y));
				Desc.Rotation          = FQuat::FromEuler(0.0f, Sim.Yaw, 0.0f);
			}
			Sim.Character     = World->CreateCharacter(Desc);
			Sim.CreatedWith   = Movement;
			Sim.bWritten      = false;
			Characters[Entity] = Sim;
			Found              = Characters.find(Entity);
		}
		FCharacterSim& Sim = Found->second;
		Sim.LastSeenFrame  = CharacterSyncCounter;
		// 스크립트/에디터가 트랜스폼을 직접 바꿨으면 순간이동 (속도 유지, 화면 오프셋은 버린다)
		if (Sim.bWritten && FVector3::DistanceSquared(Scene.GetTransform(Entity).Position, Sim.WrittenPosition) > 0.01f)
		{
			Sim.VisualOffset = FVector3();
			World->SetCharacterState(Sim.Character, Position, World->GetCharacterResult(Sim.Character).Velocity);
			Sim.WrittenPosition = Scene.GetTransform(Entity).Position;
		}
	}

	for (auto It = Characters.begin(); It != Characters.end();)
	{
		if (It->second.LastSeenFrame != CharacterSyncCounter)
		{
			World->DestroyCharacter(It->second.Character);
			It = Characters.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FPhysicsSystem::AddMovementInput(FEntity Entity, const FVector3& WorldDirection)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end() && std::isfinite(WorldDirection.X) && std::isfinite(WorldDirection.Y))
	{
		Found->second.PendingInput = FVector2(Found->second.PendingInput.X + WorldDirection.X, Found->second.PendingInput.Y + WorldDirection.Y);
	}
}

void FPhysicsSystem::RequestJump(FEntity Entity)
{
	if (const auto Found = Characters.find(Entity); Found != Characters.end())
	{
		Found->second.bPendingJump = true;
	}
}

FCharacterMove FPhysicsSystem::ConsumePendingMove(FEntity Entity, float DeltaSeconds, const float* ControlYaw)
{
	FCharacterMove Move;
	Move.DeltaSeconds = DeltaSeconds;
	const auto Found  = Characters.find(Entity);
	if (Found == Characters.end())
	{
		return Move;
	}
	FCharacterSim& Sim = Found->second;
	Move.Input         = CharacterMovementMath::ClampInput(Sim.PendingInput);
	Move.bJump         = Sim.bPendingJump;
	if (ControlYaw != nullptr && Sim.CreatedWith.bFaceControlYaw)
	{
		Move.Yaw = *ControlYaw;
	}
	else
	{
		Move.Yaw = Move.Input.X * Move.Input.X + Move.Input.Y * Move.Input.Y > 1.0e-6f ? YawFromDirection(Move.Input) : Sim.Yaw;
	}
	Sim.PendingInput = FVector2(0.0f, 0.0f);
	Sim.bPendingJump = false;
	return Move;
}

void FPhysicsSystem::SimulateCharacter(FScene& Scene, FEntity Entity, const FCharacterMove& Move)
{
	const auto Found = Characters.find(Entity);
	const FCharacterMovementComponent* Movement = Scene.GetRegistry().TryGet<FCharacterMovementComponent>(Entity);
	if (!World || Found == Characters.end() || Movement == nullptr)
	{
		return;
	}
	FCharacterSim&                Sim          = Found->second;
	const float                   DeltaSeconds = std::clamp(Move.DeltaSeconds, 0.0f, FCharacterMove::MaxMoveDeltaSeconds);
	const FPhysicsCharacterResult Before       = World->GetCharacterResult(Sim.Character);
	bool                          bJumped      = false;
	const FVector3 Velocity = CharacterMovementMath::ComputeVelocity(*Movement, Before.Velocity, Before.bGrounded, Move, World->GetGravity().Z, bJumped);
	if (std::isfinite(Move.Yaw))
	{
		Sim.Yaw = Move.Yaw;
		World->SetCharacterRotation(Sim.Character, FQuat::FromEuler(0.0f, Sim.Yaw, 0.0f));
	}
	if (DeltaSeconds > 0.0f)
	{
		// 바닥에 붙이기는 걷는 중에만 (점프/공중이면 끔 — 계단 높이만큼 아래로 당긴다)
		const float StickDown = Before.bGrounded && !bJumped ? Movement->MaxStepHeight : 0.0f;
		World->UpdateCharacter(Sim.Character, DeltaSeconds, Velocity, Movement->MaxStepHeight, StickDown);
	}
	WriteCharacterTransform(Scene, Entity);
}

FCharacterState FPhysicsSystem::GetCharacterState(FEntity Entity) const
{
	FCharacterState State;
	if (const auto Found = Characters.find(Entity); World && Found != Characters.end())
	{
		const FPhysicsCharacterResult Result = World->GetCharacterResult(Found->second.Character);
		State.Position  = Result.Position;
		State.Velocity  = Result.Velocity;
		State.bGrounded = Result.bGrounded;
	}
	return State;
}

void FPhysicsSystem::SetCharacterState(FScene& Scene, FEntity Entity, const FCharacterState& State)
{
	if (const auto Found = Characters.find(Entity); World && Found != Characters.end())
	{
		World->SetCharacterState(Found->second.Character, State.Position, State.Velocity);
		WriteCharacterTransform(Scene, Entity);
	}
}

void FPhysicsSystem::FollowTransform(FScene& Scene, FEntity Entity)
{
	if (const auto Found = Characters.find(Entity); World && Found != Characters.end())
	{
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale;
		PhysicsMath::DecomposeWorld(ComputeWorldMatrix(Scene, Entity), Position, Rotation, Scale);
		World->SetCharacterState(Found->second.Character, Position, FVector3());
		World->SetCharacterRotation(Found->second.Character, Rotation);
		Found->second.WrittenPosition = Scene.GetTransform(Entity).Position;
		Found->second.bWritten        = true;
	}
}

void FPhysicsSystem::SetCharacterVisualOffset(FScene& Scene, FEntity Entity, const FVector3& Offset)
{
	if (const auto Found = Characters.find(Entity); World && Found != Characters.end())
	{
		Found->second.VisualOffset = Offset;
		WriteCharacterTransform(Scene, Entity);
	}
}

void FPhysicsSystem::GetCharacterContacts(FEntity Entity, std::vector<FEntity>& OutEntities) const
{
	OutEntities.clear();
	const auto Found = Characters.find(Entity);
	if (!World || Found == Characters.end())
	{
		return;
	}
	std::vector<uint64> UserData;
	World->GetCharacterContacts(Found->second.Character, UserData);
	for (const uint64 Id : UserData)
	{
		OutEntities.push_back(FEntity::FromId(Id));
	}
}

float FPhysicsSystem::GetCharacterDynamicPenetration(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return World && Found != Characters.end() ? World->GetCharacterDynamicPenetration(Found->second.Character) : 0.0f;
}

void FPhysicsSystem::SetCharactersPushBodies(bool bPush)
{
	if (World)
	{
		World->SetCharactersPushBodies(bPush);
	}
}

bool FPhysicsSystem::IsGrounded(FEntity Entity) const
{
	const auto Found = Characters.find(Entity);
	return World && Found != Characters.end() && World->GetCharacterResult(Found->second.Character).bGrounded;
}

void FPhysicsSystem::WriteCharacterTransform(FScene& Scene, FEntity Entity)
{
	FCharacterSim&                Sim       = Characters.at(Entity);
	FPhysicsCharacterResult       Result    = World->GetCharacterResult(Sim.Character);
	const FQuat                   Rotation  = FQuat::FromEuler(0.0f, Sim.Yaw, 0.0f);
	Result.Position                         = Result.Position + Sim.VisualOffset;
	FTransformComponent&          Transform = Scene.GetTransform(Entity);
	if (Scene.GetParent(Entity).IsValid() || Scene.IsSocketAttached(Entity))
	{
		PhysicsMath::WorldToLocal(Scene.GetParentWorldMatrix(Entity), Result.Position, Rotation, Transform.Position, Transform.Rotation);
	}
	else
	{
		Transform.Position = Result.Position;
		Transform.Rotation = Rotation;
	}
	Sim.WrittenPosition = Transform.Position;
	Sim.bWritten        = true;
}

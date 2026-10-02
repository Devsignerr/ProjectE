#include "Physics/PhysicsSystem.h"

#include "Core/Profiling.h"
#include "Core/Settings/ProjectSettings.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/FoliageCollision.h"
#include "Physics/TerrainCollision.h"
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
			Desc.bIsTrigger  = Box->bIsTrigger;
			return true;
		}
		if (const FSphereColliderComponent* Sphere = Registry.TryGet<FSphereColliderComponent>(Entity))
		{
			Desc.Shape      = EPhysicsShape::Sphere;
			Desc.Radius     = std::abs(Sphere->Radius) * MaxAbs(MaxAbs(Scale.X, Scale.Y), Scale.Z);
			Desc.Offset     = FVector3(Sphere->Offset.X * Scale.X, Sphere->Offset.Y * Scale.Y, Sphere->Offset.Z * Scale.Z);
			Desc.bIsTrigger = Sphere->bIsTrigger;
			return true;
		}
		if (const FCapsuleColliderComponent* Capsule = Registry.TryGet<FCapsuleColliderComponent>(Entity))
		{
			Desc.Shape      = EPhysicsShape::Capsule;
			Desc.Radius     = std::abs(Capsule->Radius) * MaxAbs(Scale.X, Scale.Y);
			Desc.HalfHeight = std::abs(Capsule->HalfHeight * Scale.Z);
			Desc.Offset     = FVector3(Capsule->Offset.X * Scale.X, Capsule->Offset.Y * Scale.Y, Capsule->Offset.Z * Scale.Z);
			Desc.bIsTrigger = Capsule->bIsTrigger;
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
		    Old.bIsTrigger != New.bIsTrigger ||
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
	Joints.clear();
	Ragdolls.clear(); // 바디는 월드와 함께 사라진다
	CollisionEvents.clear();
	TerrainCollision.reset(); // 지형/폴리지 바디도 월드와 함께 사라진다
	FoliageCollision.reset();
	World.reset();
	Stepper.Reset();
}

uint32 FPhysicsSystem::Update(FScene& Scene, float DeltaSeconds)
{
	E_PROFILE_SCOPE("물리");
	if (!World)
	{
		return 0;
	}
	++FrameCounter;
	CollisionEvents.clear();
	SyncCharacters(Scene);
	SyncBodies(Scene);
	if (!TerrainCollision)
	{
		TerrainCollision = std::make_unique<FTerrainCollision>();
	}
	TerrainCollision->Sync(Scene, *World);
	if (!FoliageCollision)
	{
		FoliageCollision = std::make_unique<FFoliageCollision>();
	}
	FoliageCollision->Sync(Scene, *World);
	SyncJoints(Scene);
	SyncRagdolls(Scene);
	CollectContactEvents(); // 사라진 바디의 접촉 끝 (밖에서 부른 SyncBodies 것도)

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
		for (auto& [Model, Ragdoll] : Ragdolls)
		{
			for (FRagdollPart& Part : Ragdoll.Parts)
			{
				Part.PreviousPosition = Part.CurrentPosition;
				Part.PreviousRotation = Part.CurrentRotation;
			}
		}

		World->Step(Stepper.StepSeconds);
		CollectContactEvents();
		CheckJointBreaks();

		for (auto& [Entity, State] : Bodies)
		{
			if (State.Motion == EPhysicsMotionType::Dynamic)
			{
				World->GetTransform(State.Body, State.CurrentPosition, State.CurrentRotation);
			}
		}
		for (auto& [Model, Ragdoll] : Ragdolls)
		{
			for (FRagdollPart& Part : Ragdoll.Parts)
			{
				World->GetTransform(Part.Body, Part.CurrentPosition, Part.CurrentRotation);
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
	WriteRagdollPoses(Scene);
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
		// 접촉 보고 여부는 바디를 다시 만들지 않고 바꾼다 (NeedsRecreate 비교 대상 아님)
		const FRigidBodyComponent* ReportBody = Registry.TryGet<FRigidBodyComponent>(Entity);
		Desc.bReportContacts = Desc.bIsTrigger || (ReportBody != nullptr && ReportBody->bReportContacts) ||
		                       (ContactReportFilter && ContactReportFilter(Scene, Entity));
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
		World->SetBodyReportsContacts(State.Body, Desc.bReportContacts);
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

void FPhysicsSystem::CollectContactEvents()
{
	ContactScratch.clear();
	World->ConsumeContactEvents(ContactScratch);
	for (const FPhysicsContactEvent& Contact : ContactScratch)
	{
		const bool          bBegin = Contact.Type == EPhysicsContactEventType::Begin;
		ECollisionEventType Type   = bBegin ? ECollisionEventType::CollisionBegin : ECollisionEventType::CollisionEnd;
		if (Contact.bSensor)
		{
			Type = bBegin ? ECollisionEventType::TriggerEnter : ECollisionEventType::TriggerExit;
		}
		FCollisionEvent First;
		First.Type          = Type;
		First.Self          = FEntity::FromId(Contact.UserData1);
		First.Other         = FEntity::FromId(Contact.UserData2);
		First.Point         = Contact.Position;
		First.Normal        = -Contact.Normal; // 법선은 바디 2를 1에서 밀어내는 방향 → 1 기준으로 뒤집는다
		First.Impulse       = Contact.Impulse;
		First.ApproachSpeed = Contact.ApproachSpeed;
		FCollisionEvent Second = First;
		Second.Self            = First.Other;
		Second.Other           = First.Self;
		Second.Normal          = Contact.Normal;
		CollisionEvents.push_back(First);
		CollisionEvents.push_back(Second);
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

uint32 FPhysicsSystem::FindQueryIgnoreBody(FEntity Entity) const
{
	if (!World || !Entity.IsValid())
	{
		return FPhysicsWorld::InvalidBody;
	}
	if (const auto Body = Bodies.find(Entity); Body != Bodies.end())
	{
		return Body->second.Body;
	}
	if (const auto Character = Characters.find(Entity); Character != Characters.end())
	{
		return World->GetCharacterInnerBody(Character->second.Character);
	}
	return FPhysicsWorld::InvalidBody;
}

uint32 FPhysicsSystem::Overlap(const FPhysicsQueryShape& Shape, const FVector3& Position, const FQuat& Rotation, std::vector<FEntity>& OutEntities,
                               FEntity IgnoreEntity) const
{
	if (!World)
	{
		return 0;
	}
	std::vector<uint64> UserData;
	World->Overlap(Shape, Position, Rotation, UserData, FindQueryIgnoreBody(IgnoreEntity));
	// 엔티티 하나가 바디 여럿일 수 있다 (래그돌 캡슐 등) → 엔티티마다 한 번, 바디 순서 유지
	const size_t First = OutEntities.size();
	for (const uint64 Data : UserData)
	{
		const FEntity Entity = FEntity::FromId(Data);
		if (Entity == IgnoreEntity && IgnoreEntity.IsValid())
		{
			continue;
		}
		if (std::find(OutEntities.begin() + static_cast<std::ptrdiff_t>(First), OutEntities.end(), Entity) == OutEntities.end())
		{
			OutEntities.push_back(Entity);
		}
	}
	return static_cast<uint32>(OutEntities.size() - First);
}

uint32 FPhysicsSystem::OverlapSphere(const FVector3& Center, float Radius, std::vector<FEntity>& OutEntities, FEntity IgnoreEntity) const
{
	return Overlap(FPhysicsQueryShape::MakeSphere(Radius), Center, FQuat::Identity, OutEntities, IgnoreEntity);
}

uint32 FPhysicsSystem::OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, std::vector<FEntity>& OutEntities,
                                  FEntity IgnoreEntity) const
{
	return Overlap(FPhysicsQueryShape::MakeBox(HalfExtents), Center, Rotation, OutEntities, IgnoreEntity);
}

uint32 FPhysicsSystem::OverlapCapsule(const FVector3& Center, float Radius, float HalfHeight, const FQuat& Rotation, std::vector<FEntity>& OutEntities,
                                      FEntity IgnoreEntity) const
{
	return Overlap(FPhysicsQueryShape::MakeCapsule(Radius, HalfHeight), Center, Rotation, OutEntities, IgnoreEntity);
}

bool FPhysicsSystem::Sweep(const FPhysicsQueryShape& Shape, const FVector3& Start, const FQuat& Rotation, const FVector3& Direction, float MaxDistance,
                           FPhysicsHit& OutHit, FEntity IgnoreEntity) const
{
	FPhysicsRayHit Hit;
	if (!World || !World->Sweep(Shape, Start, Rotation, Direction, MaxDistance, Hit, FindQueryIgnoreBody(IgnoreEntity)))
	{
		return false;
	}
	OutHit.Entity   = FEntity::FromId(Hit.UserData);
	OutHit.Position = Hit.Position;
	OutHit.Normal   = Hit.Normal;
	OutHit.Distance = Hit.Distance;
	return true;
}

bool FPhysicsSystem::SphereCast(const FVector3& Start, float Radius, const FVector3& Direction, float MaxDistance, FPhysicsHit& OutHit,
                                FEntity IgnoreEntity) const
{
	return Sweep(FPhysicsQueryShape::MakeSphere(Radius), Start, FQuat::Identity, Direction, MaxDistance, OutHit, IgnoreEntity);
}

bool FPhysicsSystem::BoxCast(const FVector3& Start, const FVector3& HalfExtents, const FQuat& Rotation, const FVector3& Direction, float MaxDistance,
                             FPhysicsHit& OutHit, FEntity IgnoreEntity) const
{
	return Sweep(FPhysicsQueryShape::MakeBox(HalfExtents), Start, Rotation, Direction, MaxDistance, OutHit, IgnoreEntity);
}

bool FPhysicsSystem::CapsuleCast(const FVector3& Start, float Radius, float HalfHeight, const FQuat& Rotation, const FVector3& Direction,
                                 float MaxDistance, FPhysicsHit& OutHit, FEntity IgnoreEntity) const
{
	return Sweep(FPhysicsQueryShape::MakeCapsule(Radius, HalfHeight), Start, Rotation, Direction, MaxDistance, OutHit, IgnoreEntity);
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
		World->SetBodyReportsContacts(World->GetCharacterInnerBody(Sim.Character), ContactReportFilter && ContactReportFilter(Scene, Entity));
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

// ---------------------------------------------------------------- 관절

namespace
{
	enum class EJointKind : uint8
	{
		Fixed,
		Hinge,
		Distance,
		Ball,
	};

	// 바디 자세 + 엔티티 스케일로 로컬 점/축을 월드로
	struct FBodyFrame
	{
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale = FVector3::OneVector;

		FVector3 Point(const FVector3& Local) const { return Position + Rotation.RotateVector(FVector3(Local.X * Scale.X, Local.Y * Scale.Y, Local.Z * Scale.Z)); }
		FVector3 Direction(const FVector3& Local) const { return Rotation.RotateVector(Local).GetNormalized(); }
	};

	void AppendCommon(std::vector<float>& Out, const FVector3& Anchor, float BreakForce, bool bCollide)
	{
		Out.insert(Out.end(), { Anchor.X, Anchor.Y, Anchor.Z, BreakForce, bCollide ? 1.0f : 0.0f });
	}

	std::vector<float> MakeSignature(const FFixedJointComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		return Out;
	}
	std::vector<float> MakeSignature(const FHingeJointComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.Axis.X, Joint.Axis.Y, Joint.Axis.Z, Joint.bLimit ? 1.0f : 0.0f, Joint.MinAngle, Joint.MaxAngle,
		                        Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed, Joint.MotorMaxTorque, Joint.Friction });
		return Out;
	}
	std::vector<float> MakeSignature(const FDistanceJointComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.TargetAnchor.X, Joint.TargetAnchor.Y, Joint.TargetAnchor.Z, Joint.MinDistance, Joint.MaxDistance,
		                        Joint.SpringFrequency, Joint.SpringDamping });
		return Out;
	}
	std::vector<float> MakeSignature(const FBallJointComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.Axis.X, Joint.Axis.Y, Joint.Axis.Z, Joint.ConeAngle });
		return Out;
	}

	// 컴포넌트 → 관절 설정 (Self = 이 엔티티 바디 자세, TargetFrame = 대상 자세 — 월드면 원점)
	void FillDesc(const FFixedJointComponent& Joint, const FBodyFrame& Self, const FBodyFrame&, FPhysicsConstraintDesc& Desc)
	{
		Desc.Type   = EPhysicsConstraintType::Fixed;
		Desc.Point1 = Self.Point(Joint.Anchor);
	}
	void FillDesc(const FHingeJointComponent& Joint, const FBodyFrame& Self, const FBodyFrame&, FPhysicsConstraintDesc& Desc)
	{
		Desc.Type           = EPhysicsConstraintType::Hinge;
		Desc.Point1         = Self.Point(Joint.Anchor);
		Desc.Axis           = Self.Direction(Joint.Axis);
		Desc.bLimit         = Joint.bLimit;
		Desc.MinAngle       = FMath::DegreesToRadians(Joint.MinAngle);
		Desc.MaxAngle       = FMath::DegreesToRadians(Joint.MaxAngle);
		Desc.bMotor         = Joint.bMotor;
		Desc.MotorSpeed     = FMath::DegreesToRadians(Joint.MotorSpeed);
		Desc.MotorMaxTorque = Joint.MotorMaxTorque;
		Desc.FrictionTorque = Joint.Friction;
	}
	void FillDesc(const FDistanceJointComponent& Joint, const FBodyFrame& Self, const FBodyFrame& Target, FPhysicsConstraintDesc& Desc)
	{
		Desc.Type            = EPhysicsConstraintType::Distance;
		Desc.Point1          = Target.Point(Joint.TargetAnchor); // Body1 = 대상
		Desc.Point2          = Self.Point(Joint.Anchor);
		Desc.MinDistance     = Joint.MinDistance;
		Desc.MaxDistance     = Joint.MaxDistance;
		Desc.SpringFrequency = Joint.SpringFrequency;
		Desc.SpringDamping   = Joint.SpringDamping;
	}
	void FillDesc(const FBallJointComponent& Joint, const FBodyFrame& Self, const FBodyFrame&, FPhysicsConstraintDesc& Desc)
	{
		Desc.Type          = EPhysicsConstraintType::Cone;
		Desc.Point1        = Self.Point(Joint.Anchor);
		Desc.Axis          = Self.Direction(Joint.Axis);
		Desc.ConeHalfAngle = FMath::DegreesToRadians(FMath::Clamp(Joint.ConeAngle, 0.0f, 180.0f));
	}
} // namespace

uint32 FPhysicsSystem::GetJointCount() const
{
	uint32 Count = 0;
	for (const auto& [Key, State] : Joints)
	{
		Count += State.Constraint != FPhysicsWorld::InvalidBody ? 1u : 0u;
	}
	return Count;
}

bool FPhysicsSystem::IsJointBroken(FEntity Entity) const
{
	for (const auto& [Key, State] : Joints)
	{
		if (Key.Entity == Entity && State.bBroken)
		{
			return true;
		}
	}
	return false;
}

bool FPhysicsSystem::HasJoint(FEntity Entity) const
{
	for (const auto& [Key, State] : Joints)
	{
		if (Key.Entity == Entity && State.Constraint != FPhysicsWorld::InvalidBody)
		{
			return true;
		}
	}
	return false;
}

void FPhysicsSystem::SyncJoints(FScene& Scene)
{
	if (!World)
	{
		return;
	}
	FRegistry& Registry = Scene.GetRegistry();

	// 바디 자세 (없으면 엔티티 월드 트랜스폼 — 월드 고정 대상/바디 없는 대상)
	auto FrameOf = [&](FEntity Entity, uint32 Body) {
		FBodyFrame Frame;
		FVector3   Position;
		FQuat      Rotation;
		PhysicsMath::DecomposeWorld(ComputeWorldMatrix(Scene, Entity), Position, Rotation, Frame.Scale);
		if (Body == FPhysicsWorld::InvalidBody || !World->GetTransform(Body, Frame.Position, Frame.Rotation))
		{
			Frame.Position = Position;
			Frame.Rotation = Rotation;
		}
		return Frame;
	};
	auto BodyOf = [&](FEntity Entity) {
		if (!Entity.IsValid() || !Registry.IsValid(Entity))
		{
			return FPhysicsWorld::InvalidBody;
		}
		if (const auto Found = Bodies.find(Entity); Found != Bodies.end())
		{
			return Found->second.Body;
		}
		if (const auto Found = Characters.find(Entity); Found != Characters.end())
		{
			return World->GetCharacterInnerBody(Found->second.Character);
		}
		return FPhysicsWorld::InvalidBody;
	};

	auto SyncKind = [&]<typename TJoint>(EJointKind Kind) {
		std::vector<FEntity> Entities;
		Registry.View<TJoint>().Each([&](FEntity Entity, TJoint&) { Entities.push_back(Entity); });
		for (const FEntity Entity : Entities)
		{
			const TJoint&     Joint     = Registry.Get<TJoint>(Entity);
			const FJointKey   Key       = { Entity, static_cast<uint8>(Kind) };
			const uint32      SelfBody  = BodyOf(Entity);
			const bool        bHasTarget = Joint.Target.IsValid() && Registry.IsValid(Joint.Target) && Joint.Target != Entity;
			const uint32      TargetBody = bHasTarget ? BodyOf(Joint.Target) : FPhysicsWorld::InvalidBody;
			std::vector<float> Signature = MakeSignature(Joint);
			FJointState&      State     = Joints[Key];
			State.LastSeenFrame         = FrameCounter;
			if (State.bBroken)
			{
				continue; // 끊어진 관절은 컴포넌트를 다시 달 때까지
			}
			const bool bAlive = State.Constraint != FPhysicsWorld::InvalidBody && World->IsConstraintAlive(State.Constraint);
			const bool bSame  = State.Body1 == TargetBody && State.Body2 == SelfBody && State.Target == Joint.Target && State.Signature == Signature;
			// 실패한 설정(동적 바디 없음)은 바뀔 때까지 다시 시도하지 않는다 — 살아 있던 관절이 바디 재생성으로 사라졌으면 다시 만든다
			if (bSame && (bAlive || State.Constraint == FPhysicsWorld::InvalidBody))
			{
				continue;
			}
			if (bAlive)
			{
				World->DestroyConstraint(State.Constraint);
			}
			State.Constraint = FPhysicsWorld::InvalidBody;
			State.Body1      = TargetBody;
			State.Body2      = SelfBody;
			State.Target     = Joint.Target;
			State.Signature  = std::move(Signature);
			State.BreakForce = Joint.BreakForce;
			if (SelfBody == FPhysicsWorld::InvalidBody)
			{
				continue; // 이 엔티티에 바디가 아직 없다 (다음 동기화에서 다시)
			}
			FPhysicsConstraintDesc Desc;
			Desc.Body1             = TargetBody;
			Desc.Body2             = SelfBody;
			Desc.bCollideConnected = Joint.bCollideConnected;
			const FBodyFrame TargetFrame = bHasTarget ? FrameOf(Joint.Target, TargetBody) : FBodyFrame{};
			FillDesc(Joint, FrameOf(Entity, SelfBody), TargetFrame, Desc);
			State.Constraint = World->CreateConstraint(Desc);
		}
	};
	SyncKind.operator()<FFixedJointComponent>(EJointKind::Fixed);
	SyncKind.operator()<FHingeJointComponent>(EJointKind::Hinge);
	SyncKind.operator()<FDistanceJointComponent>(EJointKind::Distance);
	SyncKind.operator()<FBallJointComponent>(EJointKind::Ball);

	// 사라진 컴포넌트/엔티티
	for (auto It = Joints.begin(); It != Joints.end();)
	{
		if (It->second.LastSeenFrame != FrameCounter)
		{
			if (It->second.Constraint != FPhysicsWorld::InvalidBody)
			{
				World->DestroyConstraint(It->second.Constraint); // 바디와 함께 이미 사라졌으면 아무것도 하지 않는다
			}
			It = Joints.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FPhysicsSystem::CheckJointBreaks()
{
	for (auto& [Key, State] : Joints)
	{
		if (State.BreakForce <= 0.0f || State.bBroken || State.Constraint == FPhysicsWorld::InvalidBody || !World->IsConstraintAlive(State.Constraint))
		{
			continue;
		}
		const float Force = World->GetConstraintForce(State.Constraint, Stepper.StepSeconds);
		if (Force <= State.BreakForce)
		{
			continue;
		}
		World->DestroyConstraint(State.Constraint);
		State.Constraint = FPhysicsWorld::InvalidBody;
		State.bBroken    = true;
		FCollisionEvent Event;
		Event.Type    = ECollisionEventType::JointBreak;
		Event.Self    = Key.Entity;
		Event.Other   = State.Target;
		Event.Impulse = Force;
		CollisionEvents.push_back(Event);
		E_LOG(LogPhysics, Display, "관절 끊어짐: 엔티티 {} (힘 {:.0f} N > {:.0f} N)", Key.Entity.Index, Force, State.BreakForce);
	}
}

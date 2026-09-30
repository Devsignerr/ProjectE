#include "Physics/PhysicsSystem.h"

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
	Stepper.Reset();
}

void FPhysicsSystem::End()
{
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
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		return World->GetLinearVelocity(Found->second.Body);
	}
	return FVector3();
}

float FPhysicsSystem::GetMass(FEntity Entity) const
{
	if (const auto Found = Bodies.find(Entity); World && Found != Bodies.end())
	{
		return World->GetMass(Found->second.Body);
	}
	return 0.0f;
}

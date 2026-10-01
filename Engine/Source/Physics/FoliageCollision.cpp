#include "Physics/FoliageCollision.h"

#include "Core/Math/Units.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Foliage.h"
#include "Scene/Scene.h"

#pragma warning(push, 0)
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#pragma warning(pop)

#include <algorithm>

namespace
{
	constexpr JPH::ObjectLayer FoliageObjectLayer = 0; // PhysicsWorld.cpp ObjectLayers::NonMoving
} // namespace

void FFoliageCollision::Sync(FScene& Scene, FPhysicsWorld& World)
{
	++Frame;
	std::vector<FFoliageInstanceSet> Sets;
	GatherFoliage(Scene, Sets);
	JPH::BodyInterface& BodyInterface = World.GetJoltBodyInterface();
	for (const FFoliageInstanceSet& Set : Sets)
	{
		FBody& Entry   = Bodies[Set.Entity];
		Entry.LastSeen = Frame;
		if (Entry.Asset == Set.Asset && Entry.ChangeCounter == Set.Asset->ChangeCounter)
		{
			continue;
		}
		World.DestroyBody(Entry.Body);
		Entry.Body          = ~0u;
		Entry.Asset         = Set.Asset;
		Entry.ChangeCounter = Set.Asset->ChangeCounter;

		// 캡슐: 엔진 +Z 축. Jolt 캡슐은 +Y 축이므로 X축 +90° (PhysicsWorld.cpp와 같은 회전)
		JPH::StaticCompoundShapeSettings Compound;
		const JPH::Quat                  Upright = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * FMath::Pi);
		uint32                           Count   = 0;
		for (size_t TypeIndex = 0; TypeIndex < Set.Asset->Types.size(); ++TypeIndex)
		{
			const FFoliageType& Type = Set.Asset->Types[TypeIndex];
			if (!Type.bCollision)
			{
				continue;
			}
			for (const FFoliageInstance& Instance : Set.Asset->Instances[TypeIndex])
			{
				const float Radius     = std::max(Type.CollisionRadius * Instance.Scale, 1.0f) * FUnits::UnitsToMeters;
				const float FullHeight = std::max(Type.CollisionHeight * Instance.Scale, 2.0f * Type.CollisionRadius * Instance.Scale) * FUnits::UnitsToMeters;
				const float HalfHeight = std::max(0.5f * FullHeight - Radius, 0.001f);
				const FVector3 Center  = (Instance.Position + FVector3(0.0f, 0.0f, Type.ZOffset * Instance.Scale)) * FUnits::UnitsToMeters +
				                        FVector3(0.0f, 0.0f, 0.5f * FullHeight);
				Compound.AddShape(JPH::Vec3(Center.X, Center.Y, Center.Z), Upright, new JPH::CapsuleShapeSettings(HalfHeight, Radius));
				++Count;
			}
		}
		if (Count == 0)
		{
			continue;
		}
		const JPH::ShapeSettings::ShapeResult Result = Compound.Create();
		if (Result.HasError())
		{
			E_LOG(LogPhysics, Error, "폴리지 충돌 생성 실패: {}", Result.GetError().c_str());
			continue;
		}
		JPH::BodyCreationSettings Settings(Result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, FoliageObjectLayer);
		Settings.mUserData    = Set.Entity.ToId();
		const JPH::BodyID Id  = BodyInterface.CreateAndAddBody(Settings, JPH::EActivation::DontActivate);
		if (!Id.IsInvalid())
		{
			Entry.Body = Id.GetIndexAndSequenceNumber();
			E_LOG(LogPhysics, Log, "폴리지 충돌: 캡슐 {}개 (바디 1개)", Count);
		}
	}
	for (auto It = Bodies.begin(); It != Bodies.end();)
	{
		if (It->second.LastSeen != Frame)
		{
			World.DestroyBody(It->second.Body);
			It = Bodies.erase(It);
		}
		else
		{
			++It;
		}
	}
}

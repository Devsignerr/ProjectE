#include "Physics/PhysicsWorld.h"

#include "Physics/PhysicsMath.h"

#pragma warning(push, 0)
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

E_DEFINE_LOG_CATEGORY(LogPhysics, Log)

namespace
{
	// ---- 레이어: 정적(서로 충돌 안 함) / 움직이는 것
	namespace ObjectLayers
	{
		constexpr JPH::ObjectLayer NonMoving = 0;
		constexpr JPH::ObjectLayer Moving    = 1;
	}
	namespace BroadPhaseLayers
	{
		constexpr JPH::BroadPhaseLayer NonMoving(0);
		constexpr JPH::BroadPhaseLayer Moving(1);
		constexpr JPH::uint            Count = 2;
	}

	class FBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
	{
	public:
		JPH::uint            GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
		JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer Layer) const override
		{
			return Layer == ObjectLayers::NonMoving ? BroadPhaseLayers::NonMoving : BroadPhaseLayers::Moving;
		}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer Layer) const override
		{
			return Layer == BroadPhaseLayers::NonMoving ? "NonMoving" : "Moving";
		}
#endif
	};

	class FObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer Layer, JPH::BroadPhaseLayer BroadPhase) const override
		{
			return Layer == ObjectLayers::Moving || BroadPhase == BroadPhaseLayers::Moving;
		}
	};

	class FObjectPairFilter final : public JPH::ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer A, JPH::ObjectLayer B) const override
		{
			return A == ObjectLayers::Moving || B == ObjectLayers::Moving;
		}
	};

	// ---- Jolt 전역 초기화 (월드 수 참조 카운트)
	std::mutex                    GJoltMutex;
	uint32                        GJoltRefCount = 0;
	std::unique_ptr<JPH::Factory> GJoltFactory;

	void JoltTrace(const char* Format, ...)
	{
		char    Buffer[1024];
		va_list Args;
		va_start(Args, Format);
		std::vsnprintf(Buffer, sizeof(Buffer), Format, Args);
		va_end(Args);
		E_LOG(LogPhysics, Log, "[Jolt] {}", Buffer);
	}

	void AcquireJolt()
	{
		std::scoped_lock Lock(GJoltMutex);
		if (GJoltRefCount++ == 0)
		{
			JPH::RegisterDefaultAllocator();
			JPH::Trace            = JoltTrace;
			GJoltFactory          = std::make_unique<JPH::Factory>();
			JPH::Factory::sInstance = GJoltFactory.get();
			JPH::RegisterTypes();
		}
	}

	void ReleaseJolt()
	{
		std::scoped_lock Lock(GJoltMutex);
		if (--GJoltRefCount == 0)
		{
			JPH::UnregisterTypes();
			JPH::Factory::sInstance = nullptr;
			GJoltFactory.reset();
		}
	}

	// ---- 변환 (cm ↔ m, 축/쿼터니언 성분은 그대로 — PhysicsMath.h 설명 참고)
	JPH::Vec3  ToJoltVector(const FVector3& V) { return JPH::Vec3(V.X, V.Y, V.Z); }
	JPH::RVec3 ToJoltPosition(const FVector3& Centimeters)
	{
		const FVector3 Meters = PhysicsMath::ToMeters(Centimeters);
		return JPH::RVec3(Meters.X, Meters.Y, Meters.Z);
	}
	JPH::Quat ToJoltQuat(const FQuat& Q) { return JPH::Quat(Q.X, Q.Y, Q.Z, Q.W).Normalized(); }
	FVector3  FromJoltVector(JPH::Vec3Arg V) { return FVector3(V.GetX(), V.GetY(), V.GetZ()); }
	FVector3  FromJoltPosition(JPH::RVec3Arg Meters)
	{
		return PhysicsMath::ToCentimeters(FVector3(static_cast<float>(Meters.GetX()), static_cast<float>(Meters.GetY()), static_cast<float>(Meters.GetZ())));
	}
	FQuat FromJoltQuat(JPH::QuatArg Q) { return FQuat(Q.GetX(), Q.GetY(), Q.GetZ(), Q.GetW()); }

	JPH::EMotionType ToJoltMotion(EPhysicsMotionType Type)
	{
		switch (Type)
		{
		case EPhysicsMotionType::Static:    return JPH::EMotionType::Static;
		case EPhysicsMotionType::Kinematic: return JPH::EMotionType::Kinematic;
		default:                            return JPH::EMotionType::Dynamic;
		}
	}

	JPH::RefConst<JPH::Shape> CreateShape(const FPhysicsBodyDesc& Desc)
	{
		constexpr float MinSize = 0.001f; // m (1mm) — 퇴화 모양 방지
		JPH::ShapeSettings::ShapeResult Result;
		switch (Desc.Shape)
		{
		case EPhysicsShape::Sphere:
			Result = JPH::SphereShapeSettings(std::max(Desc.Radius * FUnits::UnitsToMeters, MinSize)).Create();
			break;
		case EPhysicsShape::Capsule:
			Result = JPH::CapsuleShapeSettings(std::max(Desc.HalfHeight * FUnits::UnitsToMeters, MinSize),
			                                   std::max(Desc.Radius * FUnits::UnitsToMeters, MinSize)).Create();
			break;
		default:
		{
			const FVector3 Half(std::max(Desc.HalfExtents.X * FUnits::UnitsToMeters, MinSize),
			                    std::max(Desc.HalfExtents.Y * FUnits::UnitsToMeters, MinSize),
			                    std::max(Desc.HalfExtents.Z * FUnits::UnitsToMeters, MinSize));
			const float ConvexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({ Half.X, Half.Y, Half.Z }));
			Result = JPH::BoxShapeSettings(ToJoltVector(Half), ConvexRadius).Create();
			break;
		}
		}
		if (Result.HasError())
		{
			E_LOG(LogPhysics, Error, "콜라이더 생성 실패: {}", Result.GetError().c_str());
			return nullptr;
		}

		// 캡슐은 Jolt에서 +Y 축 → 엔진 +Z 축으로 회전 (X축 +90°: Y → Z). 오프셋도 함께 적용
		const bool bRotate = Desc.Shape == EPhysicsShape::Capsule;
		const bool bOffset = Desc.Offset.LengthSquared() > 1.0e-8f;
		if (!bRotate && !bOffset)
		{
			return Result.Get();
		}
		const JPH::Quat Rotation = bRotate ? JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * FMath::Pi) : JPH::Quat::sIdentity();
		JPH::RotatedTranslatedShapeSettings Wrapped(ToJoltVector(PhysicsMath::ToMeters(Desc.Offset)), Rotation, Result.Get());
		JPH::ShapeSettings::ShapeResult     WrappedResult = Wrapped.Create();
		if (WrappedResult.HasError())
		{
			E_LOG(LogPhysics, Error, "콜라이더 오프셋/회전 적용 실패: {}", WrappedResult.GetError().c_str());
			return nullptr;
		}
		return WrappedResult.Get();
	}
} // namespace

struct FPhysicsWorld::FImpl
{
	FBroadPhaseLayers                          BroadPhaseLayers;
	FObjectVsBroadPhaseFilter                  ObjectVsBroadPhase;
	FObjectPairFilter                          ObjectPairs;
	std::unique_ptr<JPH::TempAllocatorImpl>    TempAllocator;
	std::unique_ptr<JPH::JobSystemThreadPool>  JobSystem;
	std::unique_ptr<JPH::PhysicsSystem>        System;

	JPH::BodyInterface& Bodies() { return System->GetBodyInterface(); }
	const JPH::BodyInterface& Bodies() const { return System->GetBodyInterface(); }
};

FPhysicsWorld::FPhysicsWorld()
	: Impl(std::make_unique<FImpl>())
{
	AcquireJolt();

	constexpr JPH::uint MaxBodies             = 16384;
	constexpr JPH::uint NumBodyMutexes        = 0; // 기본값
	constexpr JPH::uint MaxBodyPairs          = 65536;
	constexpr JPH::uint MaxContactConstraints = 16384;

	const uint32 WorkerThreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u));
	Impl->TempAllocator        = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
	Impl->JobSystem            = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(WorkerThreads));
	Impl->System               = std::make_unique<JPH::PhysicsSystem>();
	Impl->System->Init(MaxBodies, NumBodyMutexes, MaxBodyPairs, MaxContactConstraints, Impl->BroadPhaseLayers, Impl->ObjectVsBroadPhase,
	                   Impl->ObjectPairs);
	// 침투 허용치: Jolt 기본 2cm는 cm 단위 장면에서 물체가 바닥에 눈에 띄게 박혀 보이므로 5mm로 줄인다
	JPH::PhysicsSettings Settings = Impl->System->GetPhysicsSettings();
	Settings.mPenetrationSlop     = 0.005f;
	Impl->System->SetPhysicsSettings(Settings);
	SetGravity(FVector3(0.0f, 0.0f, -FUnits::StandardGravity));
	E_LOG(LogPhysics, Log, "물리 월드 생성 (Jolt, 작업 스레드 {}개)", WorkerThreads);
}

FPhysicsWorld::~FPhysicsWorld()
{
	// 바디 → 시스템 → 작업/임시 할당기 순으로 해제한 뒤 Jolt 전역 해제
	if (Impl->System)
	{
		JPH::BodyIDVector BodyIds;
		Impl->System->GetBodies(BodyIds);
		if (!BodyIds.empty())
		{
			Impl->Bodies().RemoveBodies(BodyIds.data(), static_cast<int>(BodyIds.size()));
			Impl->Bodies().DestroyBodies(BodyIds.data(), static_cast<int>(BodyIds.size()));
		}
	}
	Impl->System.reset();
	Impl->JobSystem.reset();
	Impl->TempAllocator.reset();
	ReleaseJolt();
}

uint32 FPhysicsWorld::CreateBody(const FPhysicsBodyDesc& Desc)
{
	const JPH::RefConst<JPH::Shape> Shape = CreateShape(Desc);
	if (Shape == nullptr)
	{
		return InvalidBody;
	}

	const JPH::EMotionType  Motion = ToJoltMotion(Desc.MotionType);
	JPH::BodyCreationSettings Settings(Shape, ToJoltPosition(Desc.Position), ToJoltQuat(Desc.Rotation), Motion,
	                                   Motion == JPH::EMotionType::Static ? ObjectLayers::NonMoving : ObjectLayers::Moving);
	Settings.mFriction       = Desc.Friction;
	Settings.mRestitution    = Desc.Restitution;
	Settings.mLinearDamping  = Desc.LinearDamping;
	Settings.mAngularDamping = Desc.AngularDamping;
	Settings.mGravityFactor  = Desc.bUseGravity ? 1.0f : 0.0f;
	Settings.mUserData       = Desc.UserData;
	if (Motion == JPH::EMotionType::Dynamic)
	{
		Settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
		Settings.mMassPropertiesOverride.mMass = std::max(Desc.Mass, 0.001f);
	}

	const JPH::BodyID Id = Impl->Bodies().CreateAndAddBody(Settings, Motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
	if (Id.IsInvalid())
	{
		E_LOG(LogPhysics, Error, "바디 생성 실패 (최대 바디 수 초과?)");
		return InvalidBody;
	}
	return Id.GetIndexAndSequenceNumber();
}

void FPhysicsWorld::DestroyBody(uint32 Body)
{
	if (Body == InvalidBody)
	{
		return;
	}
	const JPH::BodyID Id(Body);
	Impl->Bodies().RemoveBody(Id);
	Impl->Bodies().DestroyBody(Id);
}

uint32 FPhysicsWorld::GetBodyCount() const
{
	return Impl->System->GetNumBodies();
}

void FPhysicsWorld::SetTransform(uint32 Body, const FVector3& Position, const FQuat& Rotation)
{
	if (Body != InvalidBody)
	{
		Impl->Bodies().SetPositionAndRotation(JPH::BodyID(Body), ToJoltPosition(Position), ToJoltQuat(Rotation), JPH::EActivation::Activate);
	}
}

void FPhysicsWorld::MoveKinematic(uint32 Body, const FVector3& Position, const FQuat& Rotation, float DeltaSeconds)
{
	if (Body != InvalidBody && DeltaSeconds > 0.0f)
	{
		Impl->Bodies().MoveKinematic(JPH::BodyID(Body), ToJoltPosition(Position), ToJoltQuat(Rotation), DeltaSeconds);
	}
}

bool FPhysicsWorld::GetTransform(uint32 Body, FVector3& OutPosition, FQuat& OutRotation) const
{
	if (Body == InvalidBody)
	{
		return false;
	}
	JPH::RVec3 Position;
	JPH::Quat  Rotation;
	Impl->Bodies().GetPositionAndRotation(JPH::BodyID(Body), Position, Rotation);
	OutPosition = FromJoltPosition(Position);
	OutRotation = FromJoltQuat(Rotation);
	return true;
}

void FPhysicsWorld::AddForce(uint32 Body, const FVector3& Force)
{
	if (Body != InvalidBody)
	{
		Impl->Bodies().AddForce(JPH::BodyID(Body), ToJoltVector(PhysicsMath::ToMeters(Force)));
	}
}

void FPhysicsWorld::AddImpulse(uint32 Body, const FVector3& Impulse)
{
	if (Body != InvalidBody)
	{
		Impl->Bodies().AddImpulse(JPH::BodyID(Body), ToJoltVector(PhysicsMath::ToMeters(Impulse)));
	}
}

void FPhysicsWorld::SetLinearVelocity(uint32 Body, const FVector3& Velocity)
{
	if (Body != InvalidBody)
	{
		Impl->Bodies().SetLinearVelocity(JPH::BodyID(Body), ToJoltVector(PhysicsMath::ToMeters(Velocity)));
	}
}

FVector3 FPhysicsWorld::GetLinearVelocity(uint32 Body) const
{
	if (Body == InvalidBody)
	{
		return FVector3();
	}
	return PhysicsMath::ToCentimeters(FromJoltVector(Impl->Bodies().GetLinearVelocity(JPH::BodyID(Body))));
}

void FPhysicsWorld::Step(float DeltaSeconds)
{
	const JPH::EPhysicsUpdateError Error = Impl->System->Update(DeltaSeconds, 1, Impl->TempAllocator.get(), Impl->JobSystem.get());
	if (Error != JPH::EPhysicsUpdateError::None)
	{
		E_LOG(LogPhysics, Warning, "물리 스텝 경고 (코드 {}): 바디 쌍/접촉 제한 초과", static_cast<uint32>(Error));
	}
}

bool FPhysicsWorld::Raycast(const FVector3& Origin, const FVector3& Direction, float MaxDistance, FPhysicsRayHit& OutHit) const
{
	const FVector3 Normalized = Direction.GetNormalized();
	if (Normalized.LengthSquared() < 0.5f || MaxDistance <= 0.0f)
	{
		return false;
	}

	const JPH::RRayCast Ray(ToJoltPosition(Origin), ToJoltVector(PhysicsMath::ToMeters(Normalized * MaxDistance)));
	JPH::RayCastResult  Result;
	if (!Impl->System->GetNarrowPhaseQuery().CastRay(Ray, Result))
	{
		return false;
	}

	const JPH::RVec3   Point = Ray.GetPointOnRay(Result.mFraction);
	JPH::BodyLockRead Lock(Impl->System->GetBodyLockInterface(), Result.mBodyID);
	if (!Lock.Succeeded())
	{
		return false;
	}
	const JPH::Body& Body = Lock.GetBody();
	OutHit.UserData = Body.GetUserData();
	OutHit.Position = FromJoltPosition(Point);
	OutHit.Normal   = FromJoltVector(Body.GetWorldSpaceSurfaceNormal(Result.mSubShapeID2, Point));
	OutHit.Distance = Result.mFraction * MaxDistance;
	return true;
}

void FPhysicsWorld::SetGravity(const FVector3& Gravity)
{
	Impl->System->SetGravity(ToJoltVector(PhysicsMath::ToMeters(Gravity)));
}

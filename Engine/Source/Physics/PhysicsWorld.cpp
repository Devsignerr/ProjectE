#include "Physics/PhysicsWorld.h"

#include "Physics/PhysicsMath.h"

#pragma warning(push, 0)
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
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
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

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
		constexpr float MinSize    = 0.001f; // m (1mm) — 퇴화 모양 방지
		constexpr float MinDensity = 0.01f;  // kg/m³
		const float     Density    = std::max(Desc.Density, MinDensity); // 질량을 직접 지정하지 않으면 부피 × 밀도
		JPH::ShapeSettings::ShapeResult Result;
		switch (Desc.Shape)
		{
		case EPhysicsShape::Sphere:
		{
			JPH::SphereShapeSettings Sphere(std::max(Desc.Radius * FUnits::UnitsToMeters, MinSize));
			Sphere.SetDensity(Density);
			Result = Sphere.Create();
			break;
		}
		case EPhysicsShape::Capsule:
		{
			JPH::CapsuleShapeSettings Capsule(std::max(Desc.HalfHeight * FUnits::UnitsToMeters, MinSize),
			                                  std::max(Desc.Radius * FUnits::UnitsToMeters, MinSize));
			Capsule.SetDensity(Density);
			Result = Capsule.Create();
			break;
		}
		default:
		{
			const FVector3 Half(std::max(Desc.HalfExtents.X * FUnits::UnitsToMeters, MinSize),
			                    std::max(Desc.HalfExtents.Y * FUnits::UnitsToMeters, MinSize),
			                    std::max(Desc.HalfExtents.Z * FUnits::UnitsToMeters, MinSize));
			const float ConvexRadius = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({ Half.X, Half.Y, Half.Z }));
			JPH::BoxShapeSettings Box(ToJoltVector(Half), ConvexRadius);
			Box.SetDensity(Density);
			Result = Box.Create();
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

	// 구르기 저항의 굴림 반지름 (m): 구/캡슐은 반지름, 박스는 가장 짧은 반 크기
	float GetRollingRadius(const FPhysicsBodyDesc& Desc)
	{
		constexpr float MinRadius = 0.001f;
		switch (Desc.Shape)
		{
		case EPhysicsShape::Sphere:
		case EPhysicsShape::Capsule:
			return std::max(Desc.Radius * FUnits::UnitsToMeters, MinRadius);
		default:
			return std::max(std::min({ Desc.HalfExtents.X, Desc.HalfExtents.Y, Desc.HalfExtents.Z }) * FUnits::UnitsToMeters, MinRadius);
		}
	}

	// 스텝마다 접촉 중인 동적 바디를 기록한다. 콜백은 Jolt 작업 스레드에서 동시에 불리므로 바디 인덱스별 원자 변수에 스텝 번호를 쓴다
	class FContactTracker final : public JPH::ContactListener
	{
	public:
		explicit FContactTracker(JPH::uint MaxBodies)
			: LastContactStep(std::make_unique<std::atomic<uint32>[]>(MaxBodies))
			, Capacity(MaxBodies)
		{
		}

		void OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold&, JPH::ContactSettings&) override
		{
			Mark(Body1, Body2);
		}
		void OnContactPersisted(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold&, JPH::ContactSettings&) override
		{
			Mark(Body1, Body2);
		}

		// Update 전에 호출 (메인 스레드). 0은 "접촉 없음"이므로 1부터 센다
		void BeginStep() { StepIndex = StepIndex == ~0u ? 1u : StepIndex + 1u; }
		bool WasInContact(JPH::BodyID Body) const
		{
			const JPH::uint Index = Body.GetIndex();
			return Index < Capacity && LastContactStep[Index].load(std::memory_order_relaxed) == StepIndex;
		}

	private:
		void Mark(const JPH::Body& Body1, const JPH::Body& Body2)
		{
			for (const JPH::Body* Body : { &Body1, &Body2 })
			{
				const JPH::uint Index = Body->GetID().GetIndex();
				if (Body->IsDynamic() && Index < Capacity)
				{
					LastContactStep[Index].store(StepIndex, std::memory_order_relaxed);
				}
			}
		}

		std::unique_ptr<std::atomic<uint32>[]> LastContactStep;
		JPH::uint                              Capacity  = 0;
		uint32                                 StepIndex = 0;
	};

	struct FRollingBody
	{
		float Coefficient = 0.0f;
		float Radius      = 0.0f; // m
	};

	// 구르기 저항 계수 → 각감속 배율. 회전만 줄이면 마찰이 선속도를 끌어내리는데, 속이 찬 구(I = 2/5 m r²)의
	// 선감속이 계수 × g가 되려면 각감속을 (I + m r²) / I = 3.5배로 줘야 한다
	constexpr float RollingCouplingFactor = 3.5f;
} // namespace

struct FPhysicsWorld::FImpl
{
	FBroadPhaseLayers                          BroadPhaseLayers;
	FObjectVsBroadPhaseFilter                  ObjectVsBroadPhase;
	FObjectPairFilter                          ObjectPairs;
	std::unique_ptr<JPH::TempAllocatorImpl>    TempAllocator;
	std::unique_ptr<JPH::JobSystemThreadPool>  JobSystem;
	std::unique_ptr<JPH::PhysicsSystem>        System;
	std::unique_ptr<FContactTracker>           Contacts;
	std::unordered_map<uint32, FRollingBody>   RollingBodies; // 바디 ID(인덱스+시퀀스) → 구르기 저항
	std::unordered_map<uint32, JPH::Ref<JPH::CharacterVirtual>> Characters; // 캐릭터 ID → CharacterVirtual (내부 바디 포함)
	uint32                                     NextCharacterId = 1;

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
	Impl->Contacts = std::make_unique<FContactTracker>(MaxBodies);
	Impl->System->SetContactListener(Impl->Contacts.get());
	// 침투 허용치: Jolt 기본 2cm는 cm 단위 장면에서 물체가 바닥에 눈에 띄게 박혀 보이므로 5mm로 줄인다
	JPH::PhysicsSettings Settings = Impl->System->GetPhysicsSettings();
	Settings.mPenetrationSlop     = 0.005f;
	Impl->System->SetPhysicsSettings(Settings);
	SetGravity(FVector3(0.0f, 0.0f, -FUnits::StandardGravity));
	E_LOG(LogPhysics, Log, "물리 월드 생성 (Jolt, 작업 스레드 {}개)", WorkerThreads);
}

FPhysicsWorld::~FPhysicsWorld()
{
	// 캐릭터(내부 바디를 지운다) → 바디 → 시스템 → 작업/임시 할당기 순으로 해제한 뒤 Jolt 전역 해제
	Impl->Characters.clear();
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
	if (Motion == JPH::EMotionType::Dynamic && Desc.bLockRotation)
	{
		Settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
	}
	if (Motion == JPH::EMotionType::Dynamic && Desc.Mass > 0.0f)
	{
		// 질량 직접 지정: 관성은 모양에서 계산해 질량에 맞춘다 (0이면 Jolt 기본 = 모양 부피 × 밀도)
		Settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
		Settings.mMassPropertiesOverride.mMass = std::max(Desc.Mass, 0.001f);
	}

	const JPH::BodyID Id = Impl->Bodies().CreateAndAddBody(Settings, Motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
	if (Id.IsInvalid())
	{
		E_LOG(LogPhysics, Error, "바디 생성 실패 (최대 바디 수 초과?)");
		return InvalidBody;
	}
	if (Motion == JPH::EMotionType::Dynamic && Desc.RollingResistance > 0.0f)
	{
		Impl->RollingBodies[Id.GetIndexAndSequenceNumber()] = { Desc.RollingResistance, GetRollingRadius(Desc) };
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
	Impl->RollingBodies.erase(Body);
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

float FPhysicsWorld::GetMass(uint32 Body) const
{
	if (Body == InvalidBody)
	{
		return 0.0f;
	}
	JPH::BodyLockRead Lock(Impl->System->GetBodyLockInterface(), JPH::BodyID(Body));
	if (!Lock.Succeeded() || !Lock.GetBody().IsDynamic())
	{
		return 0.0f;
	}
	const float InverseMass = Lock.GetBody().GetMotionProperties()->GetInverseMass();
	return InverseMass > 0.0f ? 1.0f / InverseMass : 0.0f;
}

void FPhysicsWorld::Step(float DeltaSeconds)
{
	Impl->Contacts->BeginStep();
	const JPH::EPhysicsUpdateError Error = Impl->System->Update(DeltaSeconds, 1, Impl->TempAllocator.get(), Impl->JobSystem.get());
	if (Error != JPH::EPhysicsUpdateError::None)
	{
		E_LOG(LogPhysics, Warning, "물리 스텝 경고 (코드 {}): 바디 쌍/접촉 제한 초과", static_cast<uint32>(Error));
	}

	// 구르기 저항: Jolt에는 없으므로 이번 스텝에 접촉한 바디의 회전 속력을 일정하게 줄인다 (공중 회전에는 영향 없음)
	const float Gravity = Impl->System->GetGravity().Length(); // m/s²
	if (Gravity <= 0.0f)
	{
		return;
	}
	JPH::BodyInterface& Bodies = Impl->Bodies();
	for (const auto& [Body, Rolling] : Impl->RollingBodies)
	{
		const JPH::BodyID Id(Body);
		if (!Impl->Contacts->WasInContact(Id) || !Bodies.IsActive(Id))
		{
			continue;
		}
		const JPH::Vec3 Angular = Bodies.GetAngularVelocity(Id);
		const float     Speed   = Angular.Length();
		if (Speed <= 0.0f)
		{
			continue;
		}
		const float Deceleration = Rolling.Coefficient * Gravity * RollingCouplingFactor / Rolling.Radius; // rad/s²
		const float NewSpeed     = std::max(Speed - Deceleration * DeltaSeconds, 0.0f);
		Bodies.SetAngularVelocity(Id, Angular * (NewSpeed / Speed));
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

FVector3 FPhysicsWorld::GetGravity() const
{
	return PhysicsMath::ToCentimeters(FromJoltVector(Impl->System->GetGravity()));
}

// ---------------------------------------------------------------- 캐릭터

uint32 FPhysicsWorld::CreateCharacter(const FPhysicsCharacterDesc& Desc)
{
	constexpr float MinSize = 0.01f; // m
	const float     Radius  = std::max(Desc.Radius * FUnits::UnitsToMeters, MinSize);
	const float     Half    = std::max(Desc.HalfHeight * FUnits::UnitsToMeters, MinSize);
	// Jolt 캡슐은 +Y 축 → 엔진 +Z 축 (CreateShape와 같은 회전)
	JPH::RefConst<JPH::Shape> Capsule = new JPH::CapsuleShape(Half, Radius);
	JPH::RefConst<JPH::Shape> Shape   = new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * FMath::Pi), Capsule);

	JPH::Ref<JPH::CharacterVirtualSettings> Settings = new JPH::CharacterVirtualSettings();
	Settings->mShape          = Shape;
	Settings->mInnerBodyShape = Shape; // 다른 캐릭터/동적 물체가 이 캐릭터와 부딪히게
	Settings->mInnerBodyLayer = ObjectLayers::Moving;
	Settings->mUp             = JPH::Vec3::sAxisZ();
	// 아래 반구의 중심보다 낮은 접촉만 "발밑"으로 본다 (위치 = 캡슐 중심)
	Settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), Half);
	Settings->mMaxSlopeAngle    = FMath::DegreesToRadians(std::clamp(Desc.MaxSlopeDegrees, 0.0f, 89.0f));
	Settings->mMass             = std::max(Desc.Mass, 1.0f);
	Settings->mMaxStrength      = std::max(Desc.MaxStrength, 0.0f);
	Settings->mCharacterPadding = 0.01f;

	JPH::Ref<JPH::CharacterVirtual> Character =
		new JPH::CharacterVirtual(Settings, ToJoltPosition(Desc.Position), ToJoltQuat(Desc.Rotation), Desc.UserData, Impl->System.get());
	const uint32 Id = Impl->NextCharacterId++;
	Impl->Characters.emplace(Id, Character);
	// 처음 바닥 상태
	Character->RefreshContacts(Impl->System->GetDefaultBroadPhaseLayerFilter(ObjectLayers::Moving), Impl->System->GetDefaultLayerFilter(ObjectLayers::Moving),
	                           JPH::BodyFilter(), JPH::ShapeFilter(), *Impl->TempAllocator);
	return Id;
}

void FPhysicsWorld::DestroyCharacter(uint32 Character)
{
	Impl->Characters.erase(Character); // 소멸자가 내부 바디를 지운다
}

uint32 FPhysicsWorld::GetCharacterCount() const
{
	return static_cast<uint32>(Impl->Characters.size());
}

void FPhysicsWorld::UpdateCharacter(uint32 Character, float DeltaSeconds, const FVector3& Velocity, float StepUp, float StickDown)
{
	const auto Found = Impl->Characters.find(Character);
	if (Found == Impl->Characters.end() || DeltaSeconds <= 0.0f)
	{
		return;
	}
	JPH::CharacterVirtual& Virtual = *Found->second;
	Virtual.SetLinearVelocity(ToJoltVector(PhysicsMath::ToMeters(Velocity)));

	JPH::CharacterVirtual::ExtendedUpdateSettings Settings;
	Settings.mStickToFloorStepDown = JPH::Vec3(0.0f, 0.0f, -std::max(StickDown, 0.0f) * FUnits::UnitsToMeters);
	Settings.mWalkStairsStepUp     = JPH::Vec3(0.0f, 0.0f, std::max(StepUp, 0.0f) * FUnits::UnitsToMeters);
	Virtual.ExtendedUpdate(DeltaSeconds, Impl->System->GetGravity(), Settings, Impl->System->GetDefaultBroadPhaseLayerFilter(ObjectLayers::Moving),
	                       Impl->System->GetDefaultLayerFilter(ObjectLayers::Moving), JPH::BodyFilter(), JPH::ShapeFilter(), *Impl->TempAllocator);
}

void FPhysicsWorld::SetCharacterState(uint32 Character, const FVector3& Position, const FVector3& Velocity)
{
	const auto Found = Impl->Characters.find(Character);
	if (Found == Impl->Characters.end())
	{
		return;
	}
	JPH::CharacterVirtual& Virtual = *Found->second;
	Virtual.SetPosition(ToJoltPosition(Position));
	Virtual.SetLinearVelocity(ToJoltVector(PhysicsMath::ToMeters(Velocity)));
	Virtual.RefreshContacts(Impl->System->GetDefaultBroadPhaseLayerFilter(ObjectLayers::Moving), Impl->System->GetDefaultLayerFilter(ObjectLayers::Moving),
	                        JPH::BodyFilter(), JPH::ShapeFilter(), *Impl->TempAllocator);
}

void FPhysicsWorld::SetCharacterRotation(uint32 Character, const FQuat& Rotation)
{
	if (const auto Found = Impl->Characters.find(Character); Found != Impl->Characters.end())
	{
		Found->second->SetRotation(ToJoltQuat(Rotation));
	}
}

FPhysicsCharacterResult FPhysicsWorld::GetCharacterResult(uint32 Character) const
{
	FPhysicsCharacterResult Result;
	const auto              Found = Impl->Characters.find(Character);
	if (Found == Impl->Characters.end())
	{
		return Result;
	}
	const JPH::CharacterVirtual& Virtual = *Found->second;
	Result.Position  = FromJoltPosition(Virtual.GetPosition());
	Result.Velocity  = PhysicsMath::ToCentimeters(FromJoltVector(Virtual.GetLinearVelocity()));
	Result.bGrounded = Virtual.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
	return Result;
}

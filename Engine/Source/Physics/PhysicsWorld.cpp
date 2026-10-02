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
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
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
	// ---- 레이어: 정적(서로 충돌 안 함) / 움직이는 것 / 트리거(움직이는 것과만 — 정적 바닥과 겹침 계산 안 함, 브로드페이즈는 Moving)
	namespace ObjectLayers
	{
		constexpr JPH::ObjectLayer NonMoving = 0;
		constexpr JPH::ObjectLayer Moving    = 1;
		constexpr JPH::ObjectLayer Trigger   = 2;
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
			if (Layer == ObjectLayers::Trigger)
			{
				return BroadPhase == BroadPhaseLayers::Moving;
			}
			return Layer == ObjectLayers::Moving || BroadPhase == BroadPhaseLayers::Moving;
		}
	};

	class FObjectPairFilter final : public JPH::ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer A, JPH::ObjectLayer B) const override
		{
			if (A == ObjectLayers::Trigger || B == ObjectLayers::Trigger)
			{
				return A == ObjectLayers::Moving || B == ObjectLayers::Moving; // 트리거 ↔ 움직이는 것만
			}
			return A == ObjectLayers::Moving || B == ObjectLayers::Moving;
		}
	};

	// 레이캐스트/질의에서 트리거 레이어를 뺀다 (트리거 영역은 총알·시야를 막지 않는다)
	class FIgnoreTriggerLayerFilter final : public JPH::ObjectLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer Layer) const override { return Layer != ObjectLayers::Trigger; }
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

	// 콜백에서 모은 접촉 변화 (메인 스레드가 Step 뒤에 쌍 단위로 정리)
	struct FRawContact
	{
		bool        bAdded = true;
		JPH::BodyID Body1, Body2;
		bool        bSensor = false;
		FVector3    Position; // cm
		FVector3    Normal;
		float       ApproachSpeed = 0.0f; // cm/s
		float       Impulse       = 0.0f; // kg·cm/s
	};

	// 스텝마다 접촉 중인 동적 바디를 기록한다. 콜백은 Jolt 작업 스레드에서 동시에 불리므로 바디 인덱스별 원자 변수에 스텝 번호를 쓴다.
	// 접촉 알림: 보고 대상(ReportFlags — 메인 스레드가 스텝 밖에서만 쓴다) 바디가 낀 접촉 추가/제거를 잠금 아래 모은다
	class FContactTracker final : public JPH::ContactListener
	{
	public:
		explicit FContactTracker(JPH::uint MaxBodies)
			: LastContactStep(std::make_unique<std::atomic<uint32>[]>(MaxBodies))
			, ReportFlags(MaxBodies, 0)
			, Capacity(MaxBodies)
		{
		}

		void OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold& Manifold, JPH::ContactSettings&) override
		{
			Mark(Body1, Body2);
			if (!ShouldReport(Body1.GetID(), Body2.GetID()))
			{
				return;
			}
			FRawContact Contact;
			Contact.Body1   = Body1.GetID();
			Contact.Body2   = Body2.GetID();
			Contact.bSensor = Body1.IsSensor() || Body2.IsSensor();
			if (!Contact.bSensor && !Manifold.mRelativeContactPointsOn1.empty())
			{
				JPH::Vec3 Sum = JPH::Vec3::sZero();
				for (JPH::uint Index = 0; Index < Manifold.mRelativeContactPointsOn1.size(); ++Index)
				{
					Sum += 0.5f * (Manifold.mRelativeContactPointsOn1[Index] + Manifold.mRelativeContactPointsOn2[Index]);
				}
				const JPH::RVec3 Point = Manifold.mBaseOffset + Sum / static_cast<float>(Manifold.mRelativeContactPointsOn1.size());
				const JPH::Vec3  Normal = Manifold.mWorldSpaceNormal;
				// 다가오는 속력: 바디 2가 1 쪽(-법선)으로 오는 상대 속도 (솔버 전 값)
				const float Approach = std::max((Body1.GetPointVelocity(Point) - Body2.GetPointVelocity(Point)).Dot(Normal), 0.0f); // m/s
				const float InverseMass1 = Body1.IsDynamic() ? Body1.GetMotionProperties()->GetInverseMass() : 0.0f;
				const float InverseMass2 = Body2.IsDynamic() ? Body2.GetMotionProperties()->GetInverseMass() : 0.0f;
				const float InverseSum   = InverseMass1 + InverseMass2;
				Contact.Position      = FVector3(static_cast<float>(Point.GetX()), static_cast<float>(Point.GetY()), static_cast<float>(Point.GetZ())) * FUnits::MetersToUnits;
				Contact.Normal        = FVector3(Normal.GetX(), Normal.GetY(), Normal.GetZ());
				Contact.ApproachSpeed = Approach * FUnits::MetersToUnits;
				Contact.Impulse       = InverseSum > 0.0f ? Approach / InverseSum * FUnits::MetersToUnits : 0.0f;
			}
			std::scoped_lock Lock(EventMutex);
			Events.push_back(Contact);
		}
		void OnContactPersisted(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold&, JPH::ContactSettings&) override
		{
			Mark(Body1, Body2);
		}
		void OnContactRemoved(const JPH::SubShapeIDPair& Pair) override
		{
			if (!ShouldReport(Pair.GetBody1ID(), Pair.GetBody2ID()))
			{
				return; // 제거된 바디의 인덱스가 재사용됐을 수도 있지만 메인 스레드가 쌍 목록으로 거른다
			}
			FRawContact Contact;
			Contact.bAdded = false;
			Contact.Body1  = Pair.GetBody1ID();
			Contact.Body2  = Pair.GetBody2ID();
			std::scoped_lock Lock(EventMutex);
			Events.push_back(Contact);
		}

		void SetReport(JPH::BodyID Body, bool bReport)
		{
			if (Body.GetIndex() < Capacity)
			{
				ReportFlags[Body.GetIndex()] = bReport ? 1 : 0;
			}
		}
		bool IsReporting(JPH::BodyID Body) const { return Body.GetIndex() < Capacity && ReportFlags[Body.GetIndex()] != 0; }
		// Step 뒤 메인 스레드에서
		void TakeEvents(std::vector<FRawContact>& Out)
		{
			std::scoped_lock Lock(EventMutex);
			Out.swap(Events);
			Events.clear();
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
		bool ShouldReport(JPH::BodyID Body1, JPH::BodyID Body2) const
		{
			const JPH::uint Index1 = Body1.GetIndex();
			const JPH::uint Index2 = Body2.GetIndex();
			return (Index1 < Capacity && ReportFlags[Index1] != 0) || (Index2 < Capacity && ReportFlags[Index2] != 0);
		}

		std::unique_ptr<std::atomic<uint32>[]> LastContactStep;
		std::vector<uint8>                     ReportFlags; // 바디 인덱스 → 보고 대상 (스텝 중에는 읽기만)
		std::mutex                             EventMutex;
		std::vector<FRawContact>               Events;
		JPH::uint                              Capacity  = 0;
		uint32                                 StepIndex = 0;
	};

	uint64 MakePairKey(uint32 BodyA, uint32 BodyB)
	{
		const uint32 Low  = std::min(BodyA, BodyB);
		const uint32 High = std::max(BodyA, BodyB);
		return (static_cast<uint64>(Low) << 32) | High;
	}

	// 바디 쌍 충돌 끄기 (관절로 이은 바디, 래그돌 이웃 뼈). 모든 바디의 충돌 그룹 ID = 바디 ID(인덱스+시퀀스).
	// Jolt 작업 스레드가 읽기만 하는 동안 메인 스레드는 스텝 밖에서만 고친다
	class FPairGroupFilter final : public JPH::GroupFilter
	{
	public:
		bool CanCollide(const JPH::CollisionGroup& Group1, const JPH::CollisionGroup& Group2) const override
		{
			return Disabled.empty() || !Disabled.contains(MakePairKey(Group1.GetGroupID(), Group2.GetGroupID()));
		}

		std::unordered_map<uint64, uint32> Disabled; // 쌍 → 끈 횟수
	};

	// 캐릭터 이동 질의(CharacterVirtual)에도 바디 쌍 충돌 끄기를 적용한다 (내부 바디와 꺼진 쌍 — 죽은 캐릭터 캡슐이 자기 래그돌을 밀지 않게)
	class FCharacterBodyFilter final : public JPH::BodyFilter
	{
	public:
		FCharacterBodyFilter(const FPairGroupFilter& InPairs, JPH::BodyID InInner) : Pairs(InPairs), Inner(InInner) {}
		bool ShouldCollide(const JPH::BodyID& Body) const override
		{
			return Inner.IsInvalid() || Pairs.Disabled.empty() ||
			       !Pairs.Disabled.contains(MakePairKey(Inner.GetIndexAndSequenceNumber(), Body.GetIndexAndSequenceNumber()));
		}

	private:
		const FPairGroupFilter& Pairs;
		JPH::BodyID             Inner;
	};

	// 모양 질의의 제외 바디: 무시할 바디 자신 + 그 바디와 충돌을 끈 쌍 (FCharacterBodyFilter와 같은 규칙, 자신도 뺀다)
	class FQueryBodyFilter final : public JPH::BodyFilter
	{
	public:
		FQueryBodyFilter(const FPairGroupFilter& InPairs, uint32 InIgnore) : Pairs(InPairs), Ignore(InIgnore) {}
		bool ShouldCollide(const JPH::BodyID& Body) const override
		{
			if (Ignore == FPhysicsWorld::InvalidBody)
			{
				return true;
			}
			const uint32 Id = Body.GetIndexAndSequenceNumber();
			return Id != Ignore && (Pairs.Disabled.empty() || !Pairs.Disabled.contains(MakePairKey(Ignore, Id)));
		}

	private:
		const FPairGroupFilter& Pairs;
		uint32                  Ignore;
	};

	struct FConstraintEntry
	{
		JPH::Ref<JPH::TwoBodyConstraint> Constraint;
		EPhysicsConstraintType           Type  = EPhysicsConstraintType::Fixed;
		uint32                           Body1 = ~0u; // ~0 = 월드
		uint32                           Body2 = ~0u;
		bool                             bDisabledCollision = false;
	};

	struct FRollingBody
	{
		float Coefficient = 0.0f;
		float Radius      = 0.0f; // m
	};

	// 구르기 저항 계수 → 각감속 배율. 회전만 줄이면 마찰이 선속도를 끌어내리는데, 속이 찬 구(I = 2/5 m r²)의
	// 선감속이 계수 × g가 되려면 각감속을 (I + m r²) / I = 3.5배로 줘야 한다
	constexpr float RollingCouplingFactor = 3.5f;

	// 모든 캐릭터 공용 접촉 리스너: 캐릭터가 동적 바디를 밀지(충격량) 끌 수 있게 한다 (재조정 다시 적용 중 — 같은 무브로 두 번 밀지 않게)
	class FCharacterContacts final : public JPH::CharacterContactListener
	{
	public:
		void OnContactAdded(const JPH::CharacterVirtual*, const JPH::CharacterContact&, JPH::CharacterContactSettings& ioSettings) override
		{
			ioSettings.mCanReceiveImpulses = bPushBodies;
		}
		void OnContactPersisted(const JPH::CharacterVirtual*, const JPH::CharacterContact&, JPH::CharacterContactSettings& ioSettings) override
		{
			ioSettings.mCanReceiveImpulses = bPushBodies;
		}

		bool bPushBodies = true;
	};
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
	JPH::Ref<FPairGroupFilter>                 PairFilter;    // 모든 바디의 충돌 그룹 필터
	std::unordered_map<uint32, FConstraintEntry> Constraints; // 관절 ID → 관절
	uint32                                     NextConstraintId = 1;

	void RemoveConstraintsOf(uint32 Body);
	void AssignCollisionGroup(JPH::BodyID Body) { Bodies().SetCollisionGroup(Body, JPH::CollisionGroup(PairFilter, Body.GetIndexAndSequenceNumber(), 0)); }
	// 접촉 알림 (메인 스레드): 닿아 있는 쌍(키 → 센서 쌍인가), 아직 꺼내 가지 않은 이벤트.
	// 쌍의 바디는 항상 살아 있다 (바디를 지울 때 그 쌍을 먼저 끝낸다) → UserData는 바디 인터페이스에서 읽는다
	std::unordered_map<uint64, bool>           ActivePairs;
	std::unordered_map<uint64, bool>           DormantPairs; // ActivePairs 중 둘 다 잠들어 Jolt가 접촉을 버린 쌍 → 깨어난 것을 봤는가
	std::vector<FPhysicsContactEvent>          PendingContactEvents;
	std::vector<FRawContact>                   RawScratch;

	void ProcessRawContacts();
	void EndPairsOf(uint32 Body);
	void PushEvent(EPhysicsContactEventType Type, uint32 Body1, uint32 Body2, bool bSensor, const FRawContact* Info);
	FCharacterContacts                         CharacterContacts; // 모든 캐릭터의 리스너 (Characters보다 먼저 선언 — 나중에 해제)
	std::unordered_map<uint32, JPH::Ref<JPH::CharacterVirtual>> Characters; // 캐릭터 ID → CharacterVirtual (내부 바디 포함)
	uint32                                     NextCharacterId = 1;

	JPH::BodyInterface& Bodies() { return System->GetBodyInterface(); }
	const JPH::BodyInterface& Bodies() const { return System->GetBodyInterface(); }
};

void FPhysicsWorld::FImpl::PushEvent(EPhysicsContactEventType Type, uint32 Body1, uint32 Body2, bool bSensor, const FRawContact* Info)
{
	FPhysicsContactEvent Event;
	Event.Type      = Type;
	Event.Body1     = Body1;
	Event.Body2     = Body2;
	Event.bSensor   = bSensor;
	Event.UserData1 = Bodies().GetUserData(JPH::BodyID(Body1));
	Event.UserData2 = Bodies().GetUserData(JPH::BodyID(Body2));
	if (Info != nullptr)
	{
		// 원본은 Jolt 순서(바디 1 → 2 법선) — 핸들 순서로 바꿨으면 법선을 뒤집는다
		const bool bSwapped = Info->Body1.GetIndexAndSequenceNumber() != Body1;
		Event.Position      = Info->Position;
		Event.Normal        = bSwapped ? -Info->Normal : Info->Normal;
		Event.ApproachSpeed = Info->ApproachSpeed;
		Event.Impulse       = Info->Impulse;
	}
	PendingContactEvents.push_back(Event);
}

void FPhysicsWorld::FImpl::ProcessRawContacts()
{
	Contacts->TakeEvents(RawScratch);
	for (const FRawContact& Raw : RawScratch)
	{
		const uint32 IdA  = Raw.Body1.GetIndexAndSequenceNumber();
		const uint32 IdB  = Raw.Body2.GetIndexAndSequenceNumber();
		const uint32 Low  = std::min(IdA, IdB);
		const uint32 High = std::max(IdA, IdB);
		const uint64 Key  = MakePairKey(Low, High);
		if (Raw.bAdded)
		{
			DormantPairs.erase(Key);
			if (ActivePairs.emplace(Key, Raw.bSensor).second) // 이미 닿아 있던 쌍(잠들었다 깸)은 다시 알리지 않는다
			{
				PushEvent(EPhysicsContactEventType::Begin, Low, High, Raw.bSensor, &Raw);
			}
			continue;
		}
		const auto Found = ActivePairs.find(Key);
		if (Found == ActivePairs.end())
		{
			continue; // 제거된 바디(이미 끝냄) 또는 보고 전 쌍
		}
		// Jolt는 바디가 잠들 때도 제거를 알린다 → 둘 다 깨어 있지 않으면(정적 포함) 잠든 쌍으로 두고 깨어날 때 다시 확인한다
		if (!Bodies().IsActive(JPH::BodyID(Low)) && !Bodies().IsActive(JPH::BodyID(High)))
		{
			DormantPairs[Key] = false;
			continue;
		}
		const bool bSensor = Found->second;
		ActivePairs.erase(Found);
		PushEvent(EPhysicsContactEventType::End, Low, High, bSensor, nullptr);
	}
	RawScratch.clear();

	// 잠든 쌍: 한쪽이 깨어난 뒤 온전한 스텝 하나(깨어난 스텝은 건너뜀 — 스텝 중에 깨면 그 스텝 접촉이 다 잡히지 않았을 수 있다)에서도
	// 닿지 않았으면(순간이동, 밀려남) 끝 (Jolt는 이미 제거를 알렸으므로 다시 오지 않는다)
	for (auto It = DormantPairs.begin(); It != DormantPairs.end();)
	{
		const JPH::BodyID Low(static_cast<uint32>(It->first >> 32));
		const JPH::BodyID High(static_cast<uint32>(It->first & 0xFFFFFFFFu));
		if (!Bodies().IsActive(Low) && !Bodies().IsActive(High))
		{
			It->second = false;
			++It;
			continue;
		}
		if (!It->second)
		{
			It->second = true; // 깨어남 — 다음 스텝 뒤에 확인
			++It;
			continue;
		}
		if (!System->WereBodiesInContact(Low, High))
		{
			const auto Found = ActivePairs.find(It->first);
			if (Found != ActivePairs.end())
			{
				PushEvent(EPhysicsContactEventType::End, Low.GetIndexAndSequenceNumber(), High.GetIndexAndSequenceNumber(), Found->second, nullptr);
				ActivePairs.erase(Found);
			}
		}
		It = DormantPairs.erase(It);
	}
}

void FPhysicsWorld::FImpl::EndPairsOf(uint32 Body)
{
	for (auto It = ActivePairs.begin(); It != ActivePairs.end();)
	{
		const uint32 Low  = static_cast<uint32>(It->first >> 32);
		const uint32 High = static_cast<uint32>(It->first & 0xFFFFFFFFu);
		if (Low == Body || High == Body)
		{
			PushEvent(EPhysicsContactEventType::End, Low, High, It->second, nullptr);
			DormantPairs.erase(It->first);
			It = ActivePairs.erase(It);
		}
		else
		{
			++It;
		}
	}
}

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
	Impl->PairFilter = new FPairGroupFilter(); // Jolt 참조 카운트 객체 (Ref가 소유)
	// 침투 허용치: Jolt 기본 2cm는 cm 단위 장면에서 물체가 바닥에 눈에 띄게 박혀 보이므로 5mm로 줄인다
	JPH::PhysicsSettings Settings = Impl->System->GetPhysicsSettings();
	Settings.mPenetrationSlop     = 0.005f;
	Impl->System->SetPhysicsSettings(Settings);
	SetGravity(FVector3(0.0f, 0.0f, -FUnits::StandardGravity));
	E_LOG(LogPhysics, Log, "물리 월드 생성 (Jolt, 작업 스레드 {}개)", WorkerThreads);
}

FPhysicsWorld::~FPhysicsWorld()
{
	// 관절(바디를 가리킨다) → 캐릭터(내부 바디를 지운다) → 바디 → 시스템 → 작업/임시 할당기 순으로 해제한 뒤 Jolt 전역 해제
	for (auto& [Id, Entry] : Impl->Constraints)
	{
		Impl->System->RemoveConstraint(Entry.Constraint);
	}
	Impl->Constraints.clear();
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

	// 트리거: 잠들지 않는 키네마틱 센서 (동적 트리거는 그대로 떨어지는 센서). 정적 운동 형식이어도 키네마틱으로 만들어
	// 잠든 바디·캐릭터 내부 바디(키네마틱)를 계속 감지한다 — FPhysicsSystem은 정적처럼 순간이동으로 옮긴다
	const JPH::EMotionType  Motion = Desc.bIsTrigger && Desc.MotionType != EPhysicsMotionType::Dynamic ? JPH::EMotionType::Kinematic : ToJoltMotion(Desc.MotionType);
	const JPH::ObjectLayer  Layer  = Desc.bIsTrigger ? ObjectLayers::Trigger : (Motion == JPH::EMotionType::Static ? ObjectLayers::NonMoving : ObjectLayers::Moving);
	JPH::BodyCreationSettings Settings(Shape, ToJoltPosition(Desc.Position), ToJoltQuat(Desc.Rotation), Motion, Layer);
	if (Desc.bIsTrigger)
	{
		Settings.mIsSensor                     = true;
		Settings.mCollideKinematicVsNonDynamic = true; // 키네마틱(캐릭터 내부 바디, 키네마틱 강체)도 감지
		Settings.mAllowSleeping                = false;
	}
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
	if (Motion == JPH::EMotionType::Dynamic && Desc.RollingResistance > 0.0f && !Desc.bIsTrigger)
	{
		Impl->RollingBodies[Id.GetIndexAndSequenceNumber()] = { Desc.RollingResistance, GetRollingRadius(Desc) };
	}
	Impl->Contacts->SetReport(Id, Desc.bIsTrigger || Desc.bReportContacts);
	Impl->AssignCollisionGroup(Id);
	return Id.GetIndexAndSequenceNumber();
}

void FPhysicsWorld::DestroyBody(uint32 Body)
{
	if (Body == InvalidBody)
	{
		return;
	}
	const JPH::BodyID Id(Body);
	Impl->RemoveConstraintsOf(Body);
	Impl->EndPairsOf(Body);
	Impl->Contacts->SetReport(Id, false);
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

void FPhysicsWorld::SetAngularVelocity(uint32 Body, const FVector3& RadiansPerSecond)
{
	if (Body != InvalidBody)
	{
		Impl->Bodies().SetAngularVelocity(JPH::BodyID(Body), ToJoltVector(RadiansPerSecond));
	}
}

FVector3 FPhysicsWorld::GetAngularVelocity(uint32 Body) const
{
	return Body == InvalidBody ? FVector3() : FromJoltVector(Impl->Bodies().GetAngularVelocity(JPH::BodyID(Body)));
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
	Impl->ProcessRawContacts();

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

	const JPH::RRayCast             Ray(ToJoltPosition(Origin), ToJoltVector(PhysicsMath::ToMeters(Normalized * MaxDistance)));
	JPH::RayCastResult              Result;
	const FIgnoreTriggerLayerFilter IgnoreTriggers;
	if (!Impl->System->GetNarrowPhaseQuery().CastRay(Ray, Result, JPH::BroadPhaseLayerFilter(), IgnoreTriggers))
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

FPhysicsQueryShape FPhysicsQueryShape::MakeSphere(float InRadius)
{
	FPhysicsQueryShape Shape;
	Shape.Shape  = EPhysicsShape::Sphere;
	Shape.Radius = InRadius;
	return Shape;
}

FPhysicsQueryShape FPhysicsQueryShape::MakeBox(const FVector3& InHalfExtents)
{
	FPhysicsQueryShape Shape;
	Shape.Shape       = EPhysicsShape::Box;
	Shape.HalfExtents = InHalfExtents;
	return Shape;
}

FPhysicsQueryShape FPhysicsQueryShape::MakeCapsule(float InRadius, float InHalfHeight)
{
	FPhysicsQueryShape Shape;
	Shape.Shape      = EPhysicsShape::Capsule;
	Shape.Radius     = InRadius;
	Shape.HalfHeight = InHalfHeight;
	return Shape;
}

namespace
{
	// 질의 모양 = 콜라이더와 같은 생성 경로 (캡슐 +Y → +Z 회전, 최소 크기/볼록 반지름 보정)
	JPH::RefConst<JPH::Shape> CreateQueryShape(const FPhysicsQueryShape& Query)
	{
		FPhysicsBodyDesc Desc;
		Desc.Shape       = Query.Shape;
		Desc.HalfExtents = Query.HalfExtents;
		Desc.Radius      = Query.Radius;
		Desc.HalfHeight  = Query.HalfHeight;
		return CreateShape(Desc);
	}
} // namespace

uint32 FPhysicsWorld::Overlap(const FPhysicsQueryShape& Shape, const FVector3& Position, const FQuat& Rotation, std::vector<uint64>& OutUserData,
                              uint32 IgnoreBody) const
{
	const JPH::RefConst<JPH::Shape> Query = CreateQueryShape(Shape);
	if (Query == nullptr)
	{
		return 0;
	}
	const JPH::RMat44 CenterOfMass = JPH::RMat44::sRotationTranslation(ToJoltQuat(Rotation), ToJoltPosition(Position)).PreTranslated(Query->GetCenterOfMass());
	JPH::CollideShapeSettings Settings;
	JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> Collector;
	const FIgnoreTriggerLayerFilter IgnoreTriggers;
	const FQueryBodyFilter          Bodies(*Impl->PairFilter, IgnoreBody);
	Impl->System->GetNarrowPhaseQuery().CollideShape(Query, JPH::Vec3::sReplicate(1.0f), CenterOfMass, Settings, JPH::RVec3::sZero(), Collector,
	                                                 JPH::BroadPhaseLayerFilter(), IgnoreTriggers, Bodies);

	// 바디마다 한 번 (하위 모양 여러 개가 맞을 수 있다). 바디 ID 순으로 정렬해 결과 순서를 고정한다
	std::vector<JPH::BodyID> Hits;
	Hits.reserve(Collector.mHits.size());
	for (const JPH::CollideShapeResult& Hit : Collector.mHits)
	{
		Hits.push_back(Hit.mBodyID2);
	}
	std::sort(Hits.begin(), Hits.end());
	Hits.erase(std::unique(Hits.begin(), Hits.end()), Hits.end());
	uint32 Count = 0;
	for (const JPH::BodyID Id : Hits)
	{
		JPH::BodyLockRead Lock(Impl->System->GetBodyLockInterface(), Id);
		if (Lock.Succeeded())
		{
			OutUserData.push_back(Lock.GetBody().GetUserData());
			++Count;
		}
	}
	return Count;
}

bool FPhysicsWorld::Sweep(const FPhysicsQueryShape& Shape, const FVector3& Start, const FQuat& Rotation, const FVector3& Direction, float MaxDistance,
                          FPhysicsRayHit& OutHit, uint32 IgnoreBody) const
{
	const FVector3 Normalized = Direction.GetNormalized();
	if (Normalized.LengthSquared() < 0.5f || MaxDistance <= 0.0f)
	{
		return false;
	}
	const JPH::RefConst<JPH::Shape> Query = CreateQueryShape(Shape);
	if (Query == nullptr)
	{
		return false;
	}
	const JPH::RMat44 World = JPH::RMat44::sRotationTranslation(ToJoltQuat(Rotation), ToJoltPosition(Start));
	const JPH::RShapeCast Cast = JPH::RShapeCast::sFromWorldTransform(Query, JPH::Vec3::sReplicate(1.0f), World,
	                                                                  ToJoltVector(PhysicsMath::ToMeters(Normalized * MaxDistance)));
	JPH::ShapeCastSettings Settings;
	Settings.mReturnDeepestPoint = true; // 시작부터 겹치면 가장 깊은 점/축 (법선이 의미 있게)
	JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> Collector;
	const FIgnoreTriggerLayerFilter IgnoreTriggers;
	const FQueryBodyFilter          Bodies(*Impl->PairFilter, IgnoreBody);
	Impl->System->GetNarrowPhaseQuery().CastShape(Cast, Settings, JPH::RVec3::sZero(), Collector, JPH::BroadPhaseLayerFilter(), IgnoreTriggers, Bodies);
	if (!Collector.HadHit())
	{
		return false;
	}

	const JPH::ShapeCastResult& Hit = Collector.mHit;
	JPH::BodyLockRead           Lock(Impl->System->GetBodyLockInterface(), Hit.mBodyID2);
	if (!Lock.Succeeded())
	{
		return false;
	}
	const JPH::RVec3 Point = JPH::RVec3(Hit.mContactPointOn2);
	OutHit.UserData        = Lock.GetBody().GetUserData();
	OutHit.Position        = FromJoltPosition(Point);
	// 침투 축 = 모양을 상대 쪽으로 미는 방향 → 법선은 반대 (상대 표면에서 모양 쪽)
	const JPH::Vec3 Axis = Hit.mPenetrationAxis;
	OutHit.Normal        = Axis.LengthSq() > 1.0e-12f ? FromJoltVector(-Axis.Normalized())
	                                                  : FromJoltVector(Lock.GetBody().GetWorldSpaceSurfaceNormal(Hit.mSubShapeID2, Point));
	OutHit.Distance      = std::max(Hit.mFraction, 0.0f) * MaxDistance;
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
	Character->SetListener(&Impl->CharacterContacts);
	if (!Character->GetInnerBodyID().IsInvalid())
	{
		Impl->AssignCollisionGroup(Character->GetInnerBodyID()); // 래그돌이 자기 캐릭터 캡슐과 부딪히지 않게 끌 수 있도록
	}
	const uint32 Id = Impl->NextCharacterId++;
	Impl->Characters.emplace(Id, Character);
	// 처음 바닥 상태
	Character->RefreshContacts(Impl->System->GetDefaultBroadPhaseLayerFilter(ObjectLayers::Moving), Impl->System->GetDefaultLayerFilter(ObjectLayers::Moving),
	                           JPH::BodyFilter(), JPH::ShapeFilter(), *Impl->TempAllocator);
	return Id;
}

void FPhysicsWorld::DestroyCharacter(uint32 Character)
{
	if (const uint32 Inner = GetCharacterInnerBody(Character); Inner != InvalidBody)
	{
		Impl->RemoveConstraintsOf(Inner);
		Impl->EndPairsOf(Inner);
		Impl->Contacts->SetReport(JPH::BodyID(Inner), false);
	}
	Impl->Characters.erase(Character); // 소멸자가 내부 바디를 지운다
}

uint32 FPhysicsWorld::GetCharacterInnerBody(uint32 Character) const
{
	const auto Found = Impl->Characters.find(Character);
	if (Found == Impl->Characters.end() || Found->second->GetInnerBodyID().IsInvalid())
	{
		return InvalidBody;
	}
	return Found->second->GetInnerBodyID().GetIndexAndSequenceNumber();
}

void FPhysicsWorld::SetBodyReportsContacts(uint32 Body, bool bReport)
{
	if (Body == InvalidBody || Impl->Contacts->IsReporting(JPH::BodyID(Body)) == bReport)
	{
		return;
	}
	Impl->Contacts->SetReport(JPH::BodyID(Body), bReport);
	if (!bReport)
	{
		// 끝 통지를 더는 받지 못하는 쌍(상대도 보고 대상이 아님)은 지금 끝낸다
		for (auto It = Impl->ActivePairs.begin(); It != Impl->ActivePairs.end();)
		{
			const uint32 Low   = static_cast<uint32>(It->first >> 32);
			const uint32 High  = static_cast<uint32>(It->first & 0xFFFFFFFFu);
			const uint32 Other = Low == Body ? High : (High == Body ? Low : InvalidBody);
			if (Other != InvalidBody && !Impl->Contacts->IsReporting(JPH::BodyID(Other)))
			{
				Impl->PushEvent(EPhysicsContactEventType::End, Low, High, It->second, nullptr);
				Impl->DormantPairs.erase(It->first);
				It = Impl->ActivePairs.erase(It);
			}
			else
			{
				++It;
			}
		}
	}
}

void FPhysicsWorld::ConsumeContactEvents(std::vector<FPhysicsContactEvent>& OutEvents)
{
	OutEvents.insert(OutEvents.end(), Impl->PendingContactEvents.begin(), Impl->PendingContactEvents.end());
	Impl->PendingContactEvents.clear();
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
	const FCharacterBodyFilter BodyFilter(*Impl->PairFilter, Virtual.GetInnerBodyID());
	Virtual.ExtendedUpdate(DeltaSeconds, Impl->System->GetGravity(), Settings, Impl->System->GetDefaultBroadPhaseLayerFilter(ObjectLayers::Moving),
	                       Impl->System->GetDefaultLayerFilter(ObjectLayers::Moving), BodyFilter, JPH::ShapeFilter(), *Impl->TempAllocator);
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
	const FCharacterBodyFilter BodyFilter(*Impl->PairFilter, Virtual.GetInnerBodyID());
	Virtual.RefreshContacts(Impl->System->GetDefaultBroadPhaseLayerFilter(ObjectLayers::Moving), Impl->System->GetDefaultLayerFilter(ObjectLayers::Moving),
	                        BodyFilter, JPH::ShapeFilter(), *Impl->TempAllocator);
}

void FPhysicsWorld::SetCharacterRotation(uint32 Character, const FQuat& Rotation)
{
	if (const auto Found = Impl->Characters.find(Character); Found != Impl->Characters.end())
	{
		Found->second->SetRotation(ToJoltQuat(Rotation));
	}
}

void FPhysicsWorld::SetCharactersPushBodies(bool bPush)
{
	Impl->CharacterContacts.bPushBodies = bPush;
}

void FPhysicsWorld::GetCharacterContacts(uint32 Character, std::vector<uint64>& OutUserData) const
{
	OutUserData.clear();
	const auto Found = Impl->Characters.find(Character);
	if (Found == Impl->Characters.end())
	{
		return;
	}
	for (const JPH::CharacterContact& Contact : Found->second->GetActiveContacts())
	{
		if (Contact.mHadCollision && !Contact.mBodyB.IsInvalid())
		{
			OutUserData.push_back(Contact.mUserData);
		}
	}
}

float FPhysicsWorld::GetCharacterDynamicPenetration(uint32 Character) const
{
	const auto Found = Impl->Characters.find(Character);
	float      Depth = 0.0f; // m
	if (Found != Impl->Characters.end())
	{
		for (const JPH::CharacterContact& Contact : Found->second->GetActiveContacts())
		{
			if (Contact.mMotionTypeB == JPH::EMotionType::Dynamic && !Contact.mBodyB.IsInvalid())
			{
				Depth = std::max(Depth, -Contact.mDistance);
			}
		}
	}
	return Depth * FUnits::MetersToUnits;
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

// ---------------------------------------------------------------- 관절

void FPhysicsWorld::FImpl::RemoveConstraintsOf(uint32 Body)
{
	for (auto It = Constraints.begin(); It != Constraints.end();)
	{
		if (It->second.Body1 == Body || It->second.Body2 == Body)
		{
			System->RemoveConstraint(It->second.Constraint);
			It = Constraints.erase(It);
		}
		else
		{
			++It;
		}
	}
	// 이 바디가 낀 충돌 끄기 항목 (상대 바디는 남아 있다)
	for (auto It = PairFilter->Disabled.begin(); It != PairFilter->Disabled.end();)
	{
		if (static_cast<uint32>(It->first >> 32) == Body || static_cast<uint32>(It->first & 0xFFFFFFFFu) == Body)
		{
			It = PairFilter->Disabled.erase(It);
		}
		else
		{
			++It;
		}
	}
}

namespace
{
	JPH::Vec3 SafeAxis(const FVector3& Axis, const JPH::Vec3& Fallback)
	{
		const JPH::Vec3 Value = ToJoltVector(Axis);
		return Value.LengthSq() > 1.0e-10f ? Value.Normalized() : Fallback;
	}

	// Axis에 수직인 단위 벡터 (Normal이 수직 성분을 가지면 그것을 쓴다)
	JPH::Vec3 SafeNormal(const JPH::Vec3& Axis, const FVector3& Normal)
	{
		JPH::Vec3 Value = ToJoltVector(Normal);
		Value -= Axis * Axis.Dot(Value);
		return Value.LengthSq() > 1.0e-10f ? Value.Normalized() : Axis.GetNormalizedPerpendicular();
	}
} // namespace

uint32 FPhysicsWorld::CreateConstraint(const FPhysicsConstraintDesc& Desc)
{
	const JPH::BodyID Id1 = Desc.Body1 == InvalidBody ? JPH::BodyID() : JPH::BodyID(Desc.Body1);
	const JPH::BodyID Id2 = Desc.Body2 == InvalidBody ? JPH::BodyID() : JPH::BodyID(Desc.Body2);
	JPH::BodyInterface& Bodies = Impl->Bodies();
	const bool bDynamic1 = !Id1.IsInvalid() && Bodies.IsAdded(Id1) && Bodies.GetMotionType(Id1) == JPH::EMotionType::Dynamic;
	const bool bDynamic2 = !Id2.IsInvalid() && Bodies.IsAdded(Id2) && Bodies.GetMotionType(Id2) == JPH::EMotionType::Dynamic;
	if ((!Id1.IsInvalid() && !Bodies.IsAdded(Id1)) || (!Id2.IsInvalid() && !Bodies.IsAdded(Id2)) || (!bDynamic1 && !bDynamic2) || Id1 == Id2)
	{
		return InvalidBody; // 움직일 수 있는 쪽이 없거나 없는 바디
	}

	const JPH::RVec3 Point1 = ToJoltPosition(Desc.Point1);
	const JPH::RVec3 Point2 = ToJoltPosition(Desc.Point2);
	const JPH::Vec3  Axis   = SafeAxis(Desc.Axis, JPH::Vec3::sAxisX());
	const JPH::Vec3  Normal = SafeNormal(Axis, Desc.NormalAxis);

	JPH::TwoBodyConstraint* Created = nullptr;
	switch (Desc.Type)
	{
	case EPhysicsConstraintType::Fixed:
	{
		JPH::FixedConstraintSettings Settings;
		Settings.mAutoDetectPoint = true; // 지금 상대 자세 그대로 고정
		Created                   = Bodies.CreateConstraint(&Settings, Id1, Id2);
		break;
	}
	case EPhysicsConstraintType::Hinge:
	{
		JPH::HingeConstraintSettings Settings;
		Settings.mPoint1 = Settings.mPoint2 = Point1;
		Settings.mHingeAxis1 = Settings.mHingeAxis2 = Axis;
		Settings.mNormalAxis1 = Settings.mNormalAxis2 = Normal;
		if (Desc.bLimit)
		{
			Settings.mLimitsMin = std::clamp(std::min(Desc.MinAngle, Desc.MaxAngle), -FMath::Pi, 0.0f);
			Settings.mLimitsMax = std::clamp(std::max(Desc.MinAngle, Desc.MaxAngle), 0.0f, FMath::Pi);
		}
		Settings.mMaxFrictionTorque = std::max(Desc.FrictionTorque, 0.0f);
		Settings.mMotorSettings.SetTorqueLimit(std::max(Desc.MotorMaxTorque, 0.0f));
		Created = Bodies.CreateConstraint(&Settings, Id1, Id2);
		if (Created != nullptr && Desc.bMotor)
		{
			JPH::HingeConstraint* Hinge = static_cast<JPH::HingeConstraint*>(Created);
			Hinge->SetMotorState(JPH::EMotorState::Velocity);
			Hinge->SetTargetAngularVelocity(Desc.MotorSpeed);
		}
		break;
	}
	case EPhysicsConstraintType::Distance:
	{
		JPH::DistanceConstraintSettings Settings;
		Settings.mPoint1      = Point1;
		Settings.mPoint2      = Point2;
		const float Current   = static_cast<float>((Point2 - Point1).Length()); // m
		float       Minimum   = Desc.MinDistance >= 0.0f ? Desc.MinDistance * FUnits::UnitsToMeters : Current;
		float       Maximum   = Desc.MaxDistance >= 0.0f ? Desc.MaxDistance * FUnits::UnitsToMeters : Current;
		if (Minimum > Maximum)
		{
			std::swap(Minimum, Maximum);
		}
		Settings.mMinDistance                     = Minimum;
		Settings.mMaxDistance                     = Maximum;
		Settings.mLimitsSpringSettings.mFrequency = std::max(Desc.SpringFrequency, 0.0f);
		Settings.mLimitsSpringSettings.mDamping   = std::max(Desc.SpringDamping, 0.0f);
		Created                                   = Bodies.CreateConstraint(&Settings, Id1, Id2);
		break;
	}
	case EPhysicsConstraintType::Cone:
	{
		JPH::ConeConstraintSettings Settings;
		Settings.mPoint1 = Settings.mPoint2 = Point1;
		Settings.mTwistAxis1 = Settings.mTwistAxis2 = Axis;
		Settings.mHalfConeAngle = std::clamp(Desc.ConeHalfAngle, 0.0f, FMath::Pi);
		Created                 = Bodies.CreateConstraint(&Settings, Id1, Id2);
		break;
	}
	case EPhysicsConstraintType::SwingTwist:
	{
		JPH::SwingTwistConstraintSettings Settings;
		Settings.mPosition1 = Settings.mPosition2 = Point1;
		Settings.mTwistAxis1 = Settings.mTwistAxis2 = Axis;
		Settings.mPlaneAxis1 = Settings.mPlaneAxis2 = Normal;
		Settings.mNormalHalfConeAngle = Settings.mPlaneHalfConeAngle = std::clamp(Desc.ConeHalfAngle, 0.0f, FMath::Pi);
		Settings.mTwistMinAngle     = std::clamp(std::min(Desc.TwistMin, Desc.TwistMax), -FMath::Pi, FMath::Pi);
		Settings.mTwistMaxAngle     = std::clamp(std::max(Desc.TwistMin, Desc.TwistMax), -FMath::Pi, FMath::Pi);
		Settings.mMaxFrictionTorque = std::max(Desc.FrictionTorque, 0.0f);
		Created                     = Bodies.CreateConstraint(&Settings, Id1, Id2);
		break;
	}
	}
	if (Created == nullptr)
	{
		E_LOG(LogPhysics, Warning, "관절 생성 실패 (형식 {})", static_cast<uint32>(Desc.Type));
		return InvalidBody;
	}

	FConstraintEntry Entry;
	Entry.Constraint = Created; // Ref가 소유
	Entry.Type       = Desc.Type;
	Entry.Body1      = Desc.Body1;
	Entry.Body2      = Desc.Body2;
	Impl->System->AddConstraint(Created);
	Bodies.ActivateConstraint(Created);
	if (!Desc.bCollideConnected && Desc.Body1 != InvalidBody && Desc.Body2 != InvalidBody)
	{
		DisableCollision(Desc.Body1, Desc.Body2);
		Entry.bDisabledCollision = true;
	}
	const uint32 Id = Impl->NextConstraintId++;
	Impl->Constraints.emplace(Id, std::move(Entry));
	return Id;
}

void FPhysicsWorld::DestroyConstraint(uint32 Constraint)
{
	const auto Found = Impl->Constraints.find(Constraint);
	if (Found == Impl->Constraints.end())
	{
		return;
	}
	if (Found->second.bDisabledCollision)
	{
		EnableCollision(Found->second.Body1, Found->second.Body2);
	}
	Impl->System->RemoveConstraint(Found->second.Constraint);
	// 끊긴 관절이 잡고 있던 잠든 바디를 깨운다 (끊어진 문이 공중에 멈춰 있지 않게)
	const JPH::BodyID Ids[2] = { Found->second.Constraint->GetBody1()->GetID(), Found->second.Constraint->GetBody2()->GetID() };
	for (const JPH::BodyID& Id : Ids)
	{
		if (!Id.IsInvalid() && Impl->Bodies().IsAdded(Id))
		{
			Impl->Bodies().ActivateBody(Id);
		}
	}
	Impl->Constraints.erase(Found);
}

bool FPhysicsWorld::IsConstraintAlive(uint32 Constraint) const
{
	return Impl->Constraints.contains(Constraint);
}

uint32 FPhysicsWorld::GetConstraintCount() const
{
	return static_cast<uint32>(Impl->Constraints.size());
}

float FPhysicsWorld::GetConstraintForce(uint32 Constraint, float StepSeconds) const
{
	const auto Found = Impl->Constraints.find(Constraint);
	if (Found == Impl->Constraints.end() || StepSeconds <= 0.0f)
	{
		return 0.0f;
	}
	const JPH::TwoBodyConstraint* Base   = Found->second.Constraint.GetPtr();
	float                         Lambda = 0.0f; // N·s (위치 구속 충격량)
	switch (Found->second.Type)
	{
	case EPhysicsConstraintType::Fixed:      Lambda = static_cast<const JPH::FixedConstraint*>(Base)->GetTotalLambdaPosition().Length(); break;
	case EPhysicsConstraintType::Hinge:      Lambda = static_cast<const JPH::HingeConstraint*>(Base)->GetTotalLambdaPosition().Length(); break;
	case EPhysicsConstraintType::Distance:   Lambda = std::abs(static_cast<const JPH::DistanceConstraint*>(Base)->GetTotalLambdaPosition()); break;
	case EPhysicsConstraintType::Cone:       Lambda = static_cast<const JPH::ConeConstraint*>(Base)->GetTotalLambdaPosition().Length(); break;
	case EPhysicsConstraintType::SwingTwist: Lambda = static_cast<const JPH::SwingTwistConstraint*>(Base)->GetTotalLambdaPosition().Length(); break;
	}
	return Lambda / StepSeconds;
}

void FPhysicsWorld::DisableCollision(uint32 BodyA, uint32 BodyB)
{
	if (BodyA != InvalidBody && BodyB != InvalidBody && BodyA != BodyB)
	{
		++Impl->PairFilter->Disabled[MakePairKey(BodyA, BodyB)];
	}
}

void FPhysicsWorld::EnableCollision(uint32 BodyA, uint32 BodyB)
{
	const auto Found = Impl->PairFilter->Disabled.find(MakePairKey(BodyA, BodyB));
	if (Found != Impl->PairFilter->Disabled.end() && --Found->second == 0)
	{
		Impl->PairFilter->Disabled.erase(Found);
	}
}

JPH::BodyInterface& FPhysicsWorld::GetJoltBodyInterface()
{
	return Impl->Bodies();
}

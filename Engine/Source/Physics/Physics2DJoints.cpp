// 2D 관절 동기화·끊어짐·마우스 끌기 (FPhysics2DSystem — 규칙은 Physics2DSystem.h "관절", 필드는 Physics2DComponents.h)
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/PhysicsWorld.h" // LogPhysics
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace
{
	enum class EJoint2DKind : uint8
	{
		Distance,
		Revolute,
		Prismatic,
		Weld,
		Wheel,
	};

	constexpr float UnlimitedLength = 1.0e7f; // cm (최대 길이 제한 없음)

	// 바디(없으면 엔티티) 평면 자세 + 엔티티 월드 스케일
	struct FFrame2D
	{
		FVector2 Position;
		float    Angle = 0.0f;
		FVector3 Scale = FVector3::OneVector;

		FVector2 ToWorld(const FVector2& Local) const { return Position + Physics2DMath::Rotate(Local, Angle); }
		FVector2 ToLocal(const FVector2& World) const { return Physics2DMath::Rotate(World - Position, -Angle); }
		FVector2 ScaleLocal(const FVector2& Local) const { return FVector2(Local.X * Scale.X, Local.Y * Scale.Z); }
	};

	FVector2 NormalizeAxis(const FVector2& Axis, const FVector2& Fallback)
	{
		const float Length = Axis.Length();
		return Length > 1.0e-6f ? Axis / Length : Fallback;
	}

	// 구조 시그니처 (바뀌면 관절을 다시 만든다): 연결 지점·대상 지점·축·CollideConnected
	void AppendCommon(std::vector<float>& Out, const FVector2& Anchor, bool bCollideConnected)
	{
		Out.insert(Out.end(), { Anchor.X, Anchor.Y, bCollideConnected ? 1.0f : 0.0f });
	}

	std::vector<float> MakeSignature(const FDistanceJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.TargetAnchor.X, Joint.TargetAnchor.Y });
		return Out;
	}
	std::vector<float> MakeSignature(const FRevoluteJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.bCollideConnected);
		return Out;
	}
	std::vector<float> MakeSignature(const FPrismaticJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.Axis.X, Joint.Axis.Y });
		return Out;
	}
	std::vector<float> MakeSignature(const FWeldJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.bCollideConnected);
		return Out;
	}
	std::vector<float> MakeSignature(const FWheelJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.Axis.X, Joint.Axis.Y });
		return Out;
	}

	// 실시간 시그니처 (바뀌면 다시 만들지 않고 FPhysics2DWorld::UpdateJoint): 모터·한계·스프링(목표 포함)·길이·용접 진동수
	// (BreakForce는 시스템만 쓴다)
	std::vector<float> MakeLiveSignature(const FDistanceJoint2DComponent& Joint)
	{
		return { Joint.Length,     Joint.MinLength, Joint.MaxLength, Joint.SpringFrequency, Joint.SpringDamping, Joint.bMotor ? 1.0f : 0.0f,
		         Joint.MotorSpeed, Joint.MaxMotorForce };
	}
	std::vector<float> MakeLiveSignature(const FRevoluteJoint2DComponent& Joint)
	{
		return { Joint.bLimit ? 1.0f : 0.0f, Joint.LowerAngle,      Joint.UpperAngle,    Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed,
		         Joint.MaxMotorTorque,       Joint.SpringFrequency, Joint.SpringDamping, Joint.TargetAngle };
	}
	std::vector<float> MakeLiveSignature(const FPrismaticJoint2DComponent& Joint)
	{
		return { Joint.bLimit ? 1.0f : 0.0f, Joint.LowerTranslation, Joint.UpperTranslation, Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed,
		         Joint.MaxMotorForce,        Joint.SpringFrequency,  Joint.SpringDamping,    Joint.TargetTranslation };
	}
	std::vector<float> MakeLiveSignature(const FWeldJoint2DComponent& Joint)
	{
		return { Joint.LinearFrequency, Joint.AngularFrequency, Joint.Damping };
	}
	std::vector<float> MakeLiveSignature(const FWheelJoint2DComponent& Joint)
	{
		return { Joint.SpringFrequency, Joint.SpringDamping, Joint.bLimit ? 1.0f : 0.0f, Joint.LowerTranslation, Joint.UpperTranslation,
		         Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed, Joint.MaxMotorTorque };
	}

	// 공통: Body2 로컬 지점 = 스케일 적용 Anchor, Body1 쪽 = 같은 월드 위치 (대상이 없으면 월드 평면 위치), 기준 각 = 각2 - 각1
	void FillCommon(const FVector2& Anchor, const FFrame2D& Self, const FFrame2D* Target, FPhysics2DJointDesc& Desc)
	{
		Desc.LocalAnchor2         = Self.ScaleLocal(Anchor);
		const FVector2 WorldPoint = Self.ToWorld(Desc.LocalAnchor2);
		Desc.LocalAnchor1         = Target != nullptr ? Target->ToLocal(WorldPoint) : WorldPoint;
		Desc.ReferenceAngle       = Self.Angle - (Target != nullptr ? Target->Angle : 0.0f);
	}

	// 이 엔티티 로컬 축 → Body1 로컬 축
	FVector2 AxisInBody1(const FVector2& Axis, const FVector2& Fallback, const FFrame2D& Self, const FFrame2D* Target)
	{
		const FVector2 World = Physics2DMath::Rotate(NormalizeAxis(Axis, Fallback), Self.Angle);
		return Target != nullptr ? Physics2DMath::Rotate(World, -Target->Angle) : World;
	}

	void FillDesc(const FDistanceJoint2DComponent& Joint, const FFrame2D& Self, const FFrame2D* Target, FPhysics2DJointDesc& Desc)
	{
		Desc.Type                 = EPhysics2DJoint::Distance;
		Desc.LocalAnchor2         = Self.ScaleLocal(Joint.Anchor);
		Desc.LocalAnchor1         = Target != nullptr ? Target->ScaleLocal(Joint.TargetAnchor) : Joint.TargetAnchor;
		const FVector2 World1     = Target != nullptr ? Target->ToWorld(Desc.LocalAnchor1) : Desc.LocalAnchor1;
		const FVector2 World2     = Self.ToWorld(Desc.LocalAnchor2);
		Desc.Length               = Joint.Length >= 0.0f ? Joint.Length : (World2 - World1).Length();
		Desc.bSpring              = Joint.SpringFrequency > 0.0f;
		Desc.Hertz                = Joint.SpringFrequency;
		Desc.DampingRatio         = Joint.SpringDamping;
		Desc.bLimit               = Joint.MinLength >= 0.0f || Joint.MaxLength >= 0.0f;
		Desc.Lower                = std::max(Joint.MinLength, 0.0f);
		Desc.Upper                = Joint.MaxLength >= 0.0f ? Joint.MaxLength : UnlimitedLength;
		// Box2D 거리 모터는 부드러운 관절에서만 돈다 — 모터를 켜면 스프링을 켠다 (진동수 0 = 스프링 힘 없이 범위 안에서 자유)
		Desc.bSpring              = Desc.bSpring || Joint.bMotor;
		Desc.bMotor               = Joint.bMotor;
		Desc.MotorSpeed           = Joint.MotorSpeed;
		Desc.MaxMotorForce        = Joint.MaxMotorForce;
	}
	void FillDesc(const FRevoluteJoint2DComponent& Joint, const FFrame2D& Self, const FFrame2D* Target, FPhysics2DJointDesc& Desc)
	{
		Desc.Type = EPhysics2DJoint::Revolute;
		FillCommon(Joint.Anchor, Self, Target, Desc);
		Desc.bLimit        = Joint.bLimit;
		Desc.Lower         = Joint.LowerAngle * FMath::DegToRad;
		Desc.Upper         = Joint.UpperAngle * FMath::DegToRad;
		Desc.bMotor        = Joint.bMotor;
		Desc.MotorSpeed    = Joint.MotorSpeed * FMath::DegToRad;
		Desc.MaxMotorForce = Joint.MaxMotorTorque;
		Desc.bSpring       = Joint.SpringFrequency > 0.0f;
		Desc.Hertz         = Joint.SpringFrequency;
		Desc.DampingRatio  = Joint.SpringDamping;
		Desc.SpringTarget  = Joint.TargetAngle * FMath::DegToRad;
	}
	void FillDesc(const FPrismaticJoint2DComponent& Joint, const FFrame2D& Self, const FFrame2D* Target, FPhysics2DJointDesc& Desc)
	{
		Desc.Type = EPhysics2DJoint::Prismatic;
		FillCommon(Joint.Anchor, Self, Target, Desc);
		Desc.LocalAxis1    = AxisInBody1(Joint.Axis, FVector2(1.0f, 0.0f), Self, Target);
		Desc.bLimit        = Joint.bLimit;
		Desc.Lower         = Joint.LowerTranslation;
		Desc.Upper         = Joint.UpperTranslation;
		Desc.bMotor        = Joint.bMotor;
		Desc.MotorSpeed    = Joint.MotorSpeed;
		Desc.MaxMotorForce = Joint.MaxMotorForce;
		Desc.bSpring       = Joint.SpringFrequency > 0.0f;
		Desc.Hertz         = Joint.SpringFrequency;
		Desc.DampingRatio  = Joint.SpringDamping;
		Desc.SpringTarget  = Joint.TargetTranslation;
	}
	void FillDesc(const FWeldJoint2DComponent& Joint, const FFrame2D& Self, const FFrame2D* Target, FPhysics2DJointDesc& Desc)
	{
		Desc.Type = EPhysics2DJoint::Weld;
		FillCommon(Joint.Anchor, Self, Target, Desc);
		Desc.LinearHertz         = Joint.LinearFrequency;
		Desc.AngularHertz        = Joint.AngularFrequency;
		Desc.LinearDampingRatio  = Joint.Damping;
		Desc.AngularDampingRatio = Joint.Damping;
	}
	void FillDesc(const FWheelJoint2DComponent& Joint, const FFrame2D& Self, const FFrame2D* Target, FPhysics2DJointDesc& Desc)
	{
		Desc.Type = EPhysics2DJoint::Wheel;
		FillCommon(Joint.Anchor, Self, Target, Desc);
		Desc.LocalAxis1    = AxisInBody1(Joint.Axis, FVector2(0.0f, 1.0f), Self, Target);
		Desc.bSpring       = Joint.SpringFrequency > 0.0f;
		Desc.Hertz         = Joint.SpringFrequency;
		Desc.DampingRatio  = Joint.SpringDamping;
		Desc.bLimit        = Joint.bLimit;
		Desc.Lower         = Joint.LowerTranslation;
		Desc.Upper         = Joint.UpperTranslation;
		Desc.bMotor        = Joint.bMotor;
		Desc.MotorSpeed    = Joint.MotorSpeed * FMath::DegToRad;
		Desc.MaxMotorForce = Joint.MaxMotorTorque;
	}

	// 실시간 필드만 채운 설명 (UpdateJoint — 자세가 필요한 필드는 쓰지 않는다. 거리 Length < 0 = 지금 길이 유지)
	template <typename TJoint>
	FPhysics2DJointDesc MakeLiveDesc(const TJoint& Joint)
	{
		FPhysics2DJointDesc Desc;
		const FFrame2D      Identity{};
		FillDesc(Joint, Identity, nullptr, Desc);
		if constexpr (std::is_same_v<TJoint, FDistanceJoint2DComponent>)
		{
			Desc.Length = Joint.Length > 0.0f ? Joint.Length : -1.0f;
		}
		return Desc;
	}

	template <typename TJoint>
	constexpr EJoint2DKind KindOf()
	{
		if constexpr (std::is_same_v<TJoint, FDistanceJoint2DComponent>)
		{
			return EJoint2DKind::Distance;
		}
		else if constexpr (std::is_same_v<TJoint, FRevoluteJoint2DComponent>)
		{
			return EJoint2DKind::Revolute;
		}
		else if constexpr (std::is_same_v<TJoint, FPrismaticJoint2DComponent>)
		{
			return EJoint2DKind::Prismatic;
		}
		else if constexpr (std::is_same_v<TJoint, FWeldJoint2DComponent>)
		{
			return EJoint2DKind::Weld;
		}
		else
		{
			return EJoint2DKind::Wheel;
		}
	}
} // namespace

void FPhysics2DSystem::SyncJoints(FScene& Scene)
{
	if (!World)
	{
		return;
	}
	FRegistry& Registry = Scene.GetRegistry();

	const auto FrameOf = [&](FEntity Entity, uint32 Body) {
		FFrame2D Frame;
		FVector3 Position;
		FQuat    Rotation;
		PhysicsMath::DecomposeWorld(Scene.GetTransform(Entity).GetLocalMatrix() * Scene.GetParentWorldMatrix(Entity), Position, Rotation, Frame.Scale);
		if (Body == FPhysics2DWorld::InvalidBody || !World->GetTransform(Body, Frame.Position, Frame.Angle))
		{
			Frame.Position = Physics2DMath::ToPlane(Position);
			Frame.Angle    = Physics2DMath::AngleFromRotation(Rotation);
		}
		return Frame;
	};
	const auto BodyOf = [&](FEntity Entity) {
		if (!Entity.IsValid() || !Registry.IsValid(Entity))
		{
			return FPhysics2DWorld::InvalidBody;
		}
		const auto Found = Bodies.find(Entity);
		return Found != Bodies.end() ? Found->second.Body : FPhysics2DWorld::InvalidBody;
	};
	const auto IsDynamic = [&](FEntity Entity) { return Entity.IsValid() && IsDynamicBody(Entity); };

	const auto SyncKind = [&]<typename TJoint>(EJoint2DKind Kind) {
		std::vector<FEntity> Entities;
		Registry.View<TJoint>().Each([&](FEntity Entity, TJoint&) { Entities.push_back(Entity); });
		for (const FEntity Entity : Entities)
		{
			const TJoint&      Joint      = Registry.Get<TJoint>(Entity);
			const FJointKey    Key        = { Entity, static_cast<uint8>(Kind) };
			const uint32       SelfBody   = BodyOf(Entity);
			const bool         bHasTarget = Joint.Target.IsValid() && Registry.IsValid(Joint.Target) && Joint.Target != Entity;
			const uint32       TargetBody = bHasTarget ? BodyOf(Joint.Target) : FPhysics2DWorld::InvalidBody;
			std::vector<float> Signature  = MakeSignature(Joint);
			FJointState&       State      = Joints[Key];
			State.LastSeenFrame           = FrameCounter;
			if (State.bBroken)
			{
				continue; // 끊어진 관절은 컴포넌트를 다시 달 때까지
			}
			const bool bAlive = State.Joint != FPhysics2DWorld::InvalidJoint && World->IsJointAlive(State.Joint);
			const bool bSame  = State.Body1 == TargetBody && State.Body2 == SelfBody && State.Target == Joint.Target && State.Signature == Signature;
			// 실패한 설정은 바뀔 때까지 다시 시도하지 않는다 — 살아 있던 관절이 바디 재생성으로 사라졌으면 다시 만든다
			if (bSame && (bAlive || State.Joint == FPhysics2DWorld::InvalidJoint))
			{
				// 실시간 필드만 바뀌었으면 다시 만들지 않고 옮긴다 (기준 자세 유지)
				State.BreakForce = Joint.BreakForce;
				if (bAlive)
				{
					std::vector<float> Live = MakeLiveSignature(Joint);
					if (Live != State.LiveSignature && World->UpdateJoint(State.Joint, MakeLiveDesc(Joint)))
					{
						State.LiveSignature = std::move(Live);
					}
				}
				continue;
			}
			if (bAlive)
			{
				World->DestroyJoint(State.Joint);
			}
			State.Joint      = FPhysics2DWorld::InvalidJoint;
			State.Body1      = TargetBody;
			State.Body2      = SelfBody;
			State.Target     = Joint.Target;
			State.Signature     = std::move(Signature);
			State.LiveSignature = MakeLiveSignature(Joint);
			State.BreakForce    = Joint.BreakForce;
			if (SelfBody == FPhysics2DWorld::InvalidBody || (!IsDynamic(Entity) && !(TargetBody != FPhysics2DWorld::InvalidBody && IsDynamic(Joint.Target))))
			{
				continue; // 바디가 아직 없거나 둘 다 동적이 아니다 (바디가 바뀌면 다시)
			}
			FPhysics2DJointDesc Desc;
			Desc.Body1             = TargetBody;
			Desc.Body2             = SelfBody;
			Desc.bCollideConnected = Joint.bCollideConnected;
			const FFrame2D Self    = FrameOf(Entity, SelfBody);
			const FFrame2D Target  = bHasTarget ? FrameOf(Joint.Target, TargetBody) : FFrame2D{};
			// 대상 엔티티가 있어도 바디가 없으면 월드 고정 (대상 위치 기준 지점을 월드로)
			FillDesc(Joint, Self, TargetBody != FPhysics2DWorld::InvalidBody ? &Target : nullptr, Desc);
			if constexpr (std::is_same_v<TJoint, FDistanceJoint2DComponent>)
			{
				if (bHasTarget && TargetBody == FPhysics2DWorld::InvalidBody)
				{
					Desc.LocalAnchor1 = Target.ToWorld(Target.ScaleLocal(Joint.TargetAnchor));
					if (Joint.Length < 0.0f)
					{
						Desc.Length = (Self.ToWorld(Desc.LocalAnchor2) - Desc.LocalAnchor1).Length();
					}
				}
			}
			State.Joint = World->CreateJoint(Desc);
		}
	};
	SyncKind.operator()<FDistanceJoint2DComponent>(EJoint2DKind::Distance);
	SyncKind.operator()<FRevoluteJoint2DComponent>(EJoint2DKind::Revolute);
	SyncKind.operator()<FPrismaticJoint2DComponent>(EJoint2DKind::Prismatic);
	SyncKind.operator()<FWeldJoint2DComponent>(EJoint2DKind::Weld);
	SyncKind.operator()<FWheelJoint2DComponent>(EJoint2DKind::Wheel);

	// 사라진 컴포넌트/엔티티
	for (auto It = Joints.begin(); It != Joints.end();)
	{
		if (It->second.LastSeenFrame != FrameCounter)
		{
			if (It->second.Joint != FPhysics2DWorld::InvalidJoint)
			{
				World->DestroyJoint(It->second.Joint); // 바디와 함께 이미 사라졌으면 아무것도 하지 않는다
			}
			It = Joints.erase(It);
		}
		else
		{
			++It;
		}
	}
	// 바디와 함께 사라진 끌기
	std::erase_if(Drags, [this](const auto& Entry) { return !World->IsJointAlive(Entry.second); });
}

void FPhysics2DSystem::CheckJointBreaks()
{
	for (auto& [Key, State] : Joints)
	{
		if (State.BreakForce <= 0.0f || State.bBroken || State.Joint == FPhysics2DWorld::InvalidJoint || !World->IsJointAlive(State.Joint))
		{
			continue;
		}
		const float Force = World->GetJointForce(State.Joint);
		if (Force <= State.BreakForce)
		{
			continue;
		}
		World->DestroyJoint(State.Joint);
		State.Joint   = FPhysics2DWorld::InvalidJoint;
		State.bBroken = true;
		FCollisionEvent Event;
		Event.Type    = ECollisionEventType::JointBreak;
		Event.Self    = Key.Entity;
		Event.Other   = State.Target;
		Event.Impulse = Force;
		CollisionEvents.push_back(Event);
		E_LOG(LogPhysics, Display, "2D 관절 끊어짐: 엔티티 {} (힘 {:.0f} N > {:.0f} N)", Key.Entity.Index, Force, State.BreakForce);
	}
}

bool FPhysics2DSystem::HasJoint(FEntity Entity) const
{
	for (const auto& [Key, State] : Joints)
	{
		if (Key.Entity == Entity && State.Joint != FPhysics2DWorld::InvalidJoint && World && World->IsJointAlive(State.Joint))
		{
			return true;
		}
	}
	return false;
}

bool FPhysics2DSystem::IsJointBroken(FEntity Entity) const
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

bool FPhysics2DSystem::BeginDrag(FEntity Entity, const FVector2& Point, float MaxForce)
{
	const auto Found = Bodies.find(Entity);
	if (!World || Found == Bodies.end() || Found->second.CreatedDesc.Type != EBodyType2D::Dynamic)
	{
		return false;
	}
	EndDrag(Entity);
	FPhysics2DJointDesc Desc;
	Desc.Type     = EPhysics2DJoint::Mouse;
	Desc.Body2    = Found->second.Body;
	Desc.Target   = Point;
	Desc.MaxForce = MaxForce > 0.0f ? MaxForce : World->GetMass(Found->second.Body) * 1000.0f;
	const uint32 Joint = World->CreateJoint(Desc);
	if (Joint == FPhysics2DWorld::InvalidJoint)
	{
		return false;
	}
	Drags[Entity] = Joint;
	return true;
}

bool FPhysics2DSystem::UpdateDrag(FEntity Entity, const FVector2& Target)
{
	const auto Found = Drags.find(Entity);
	if (!World || Found == Drags.end() || !World->IsJointAlive(Found->second))
	{
		return false;
	}
	World->SetMouseTarget(Found->second, Target);
	return true;
}

bool FPhysics2DSystem::EndDrag(FEntity Entity)
{
	const auto Found = Drags.find(Entity);
	if (Found == Drags.end())
	{
		return false;
	}
	if (World)
	{
		World->DestroyJoint(Found->second);
	}
	Drags.erase(Found);
	return true;
}

bool FPhysics2DSystem::IsDragging(FEntity Entity) const
{
	const auto Found = Drags.find(Entity);
	return World && Found != Drags.end() && World->IsJointAlive(Found->second);
}

// ---------------------------------------------------------------- 실시간 제어 (Physics2DSystem.h "관절" 절 — 컴포넌트 값을 바꾸고 바로 옮긴다)

void FPhysics2DSystem::ApplyJointLiveSettings(FScene& Scene, FEntity Entity)
{
	if (!World || !Scene.GetRegistry().IsValid(Entity))
	{
		return;
	}
	FRegistry& Registry = Scene.GetRegistry();
	const auto Apply    = [&]<typename TJoint>() {
		const TJoint* Joint = Registry.TryGet<TJoint>(Entity);
		const auto    Found = Joints.find(FJointKey{ Entity, static_cast<uint8>(KindOf<TJoint>()) });
		if (Joint == nullptr || Found == Joints.end())
		{
			return; // 아직 만들지 않았다 — 다음 SyncJoints가 컴포넌트 값으로 만든다
		}
		FJointState& State = Found->second;
		State.BreakForce   = Joint->BreakForce;
		if (State.Joint == FPhysics2DWorld::InvalidJoint || !World->IsJointAlive(State.Joint) || State.Signature != MakeSignature(*Joint))
		{
			return; // 구조가 바뀌었으면 SyncJoints가 다시 만든다
		}
		std::vector<float> Live = MakeLiveSignature(*Joint);
		if (Live != State.LiveSignature && World->UpdateJoint(State.Joint, MakeLiveDesc(*Joint)))
		{
			State.LiveSignature = std::move(Live);
		}
	};
	Apply.operator()<FDistanceJoint2DComponent>();
	Apply.operator()<FRevoluteJoint2DComponent>();
	Apply.operator()<FPrismaticJoint2DComponent>();
	Apply.operator()<FWeldJoint2DComponent>();
	Apply.operator()<FWheelJoint2DComponent>();
}

namespace
{
	// 엔티티의 관절 컴포넌트 중 Edit이 true를 돌려준 것이 있으면 true (Edit은 지원하는 종류만 고친다)
	template <typename TEdit>
	bool EditJointComponents(FScene& Scene, FEntity Entity, TEdit&& Edit)
	{
		FRegistry& Registry = Scene.GetRegistry();
		if (!Registry.IsValid(Entity))
		{
			return false;
		}
		bool bAny = false;
		if (FDistanceJoint2DComponent* Joint = Registry.TryGet<FDistanceJoint2DComponent>(Entity))
		{
			bAny = Edit(*Joint) || bAny;
		}
		if (FRevoluteJoint2DComponent* Joint = Registry.TryGet<FRevoluteJoint2DComponent>(Entity))
		{
			bAny = Edit(*Joint) || bAny;
		}
		if (FPrismaticJoint2DComponent* Joint = Registry.TryGet<FPrismaticJoint2DComponent>(Entity))
		{
			bAny = Edit(*Joint) || bAny;
		}
		if (FWeldJoint2DComponent* Joint = Registry.TryGet<FWeldJoint2DComponent>(Entity))
		{
			bAny = Edit(*Joint) || bAny;
		}
		if (FWheelJoint2DComponent* Joint = Registry.TryGet<FWheelJoint2DComponent>(Entity))
		{
			bAny = Edit(*Joint) || bAny;
		}
		return bAny;
	}

	// 모터: 회전·미닫이·바퀴·거리(윈치) / 한계 켜기: 회전·미닫이·바퀴 (거리는 Min/MaxLength < 0 = 없음)
	template <typename TJoint>
	constexpr bool HasMotor = std::is_same_v<TJoint, FRevoluteJoint2DComponent> || std::is_same_v<TJoint, FPrismaticJoint2DComponent> ||
	                          std::is_same_v<TJoint, FWheelJoint2DComponent> || std::is_same_v<TJoint, FDistanceJoint2DComponent>;
	template <typename TJoint>
	constexpr bool HasLimitToggle = std::is_same_v<TJoint, FRevoluteJoint2DComponent> || std::is_same_v<TJoint, FPrismaticJoint2DComponent> ||
	                                std::is_same_v<TJoint, FWheelJoint2DComponent>;
} // namespace

bool FPhysics2DSystem::SetJointMotorSpeed(FScene& Scene, FEntity Entity, float Speed)
{
	const bool bAny = std::isfinite(Speed) && EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
		if constexpr (HasMotor<TJoint>)
		{
			Joint.MotorSpeed = Speed;
			return true;
		}
		else
		{
			return false;
		}
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

bool FPhysics2DSystem::SetJointMaxMotorForce(FScene& Scene, FEntity Entity, float Force)
{
	const bool bAny = std::isfinite(Force) && EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
		if constexpr (std::is_same_v<TJoint, FPrismaticJoint2DComponent> || std::is_same_v<TJoint, FDistanceJoint2DComponent>)
		{
			Joint.MaxMotorForce = std::max(Force, 0.0f);
			return true;
		}
		else if constexpr (HasMotor<TJoint>)
		{
			Joint.MaxMotorTorque = std::max(Force, 0.0f);
			return true;
		}
		else
		{
			return false;
		}
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

bool FPhysics2DSystem::EnableJointMotor(FScene& Scene, FEntity Entity, bool bEnable)
{
	const bool bAny = EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
		if constexpr (HasMotor<TJoint>)
		{
			Joint.bMotor = bEnable;
			return true;
		}
		else
		{
			return false;
		}
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

bool FPhysics2DSystem::SetJointLimits(FScene& Scene, FEntity Entity, float Lower, float Upper)
{
	if (!std::isfinite(Lower) || !std::isfinite(Upper))
	{
		return false;
	}
	const float Low = std::min(Lower, Upper), High = std::max(Lower, Upper);
	const bool  bAny = EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
        if constexpr (std::is_same_v<TJoint, FRevoluteJoint2DComponent>)
        {
            Joint.bLimit     = true;
            Joint.LowerAngle = Low;
            Joint.UpperAngle = High;
            return true;
        }
        else if constexpr (std::is_same_v<TJoint, FPrismaticJoint2DComponent> || std::is_same_v<TJoint, FWheelJoint2DComponent>)
        {
            Joint.bLimit           = true;
            Joint.LowerTranslation = Low;
            Joint.UpperTranslation = High;
            return true;
        }
        else if constexpr (std::is_same_v<TJoint, FDistanceJoint2DComponent>)
        {
            Joint.MinLength = std::max(Low, 0.0f);
            Joint.MaxLength = std::max(High, 0.0f);
            return true;
        }
        else
        {
        	return false;
        }
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

bool FPhysics2DSystem::EnableJointLimit(FScene& Scene, FEntity Entity, bool bEnable)
{
	const bool bAny = EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
		if constexpr (HasLimitToggle<TJoint>)
		{
			Joint.bLimit = bEnable;
			return true;
		}
		else
		{
			return false;
		}
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

bool FPhysics2DSystem::SetJointSpring(FScene& Scene, FEntity Entity, float Frequency, float Damping)
{
	if (!std::isfinite(Frequency) || !std::isfinite(Damping))
	{
		return false;
	}
	const float Hertz = std::max(Frequency, 0.0f), Ratio = std::max(Damping, 0.0f);
	const bool  bAny  = EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
        if constexpr (std::is_same_v<TJoint, FDistanceJoint2DComponent> || std::is_same_v<TJoint, FWheelJoint2DComponent> ||
                      std::is_same_v<TJoint, FRevoluteJoint2DComponent> || std::is_same_v<TJoint, FPrismaticJoint2DComponent>)
        {
            Joint.SpringFrequency = Hertz;
            Joint.SpringDamping   = Ratio;
            return true;
        }
        else if constexpr (std::is_same_v<TJoint, FWeldJoint2DComponent>)
        {
            Joint.LinearFrequency  = Hertz;
            Joint.AngularFrequency = Hertz;
            Joint.Damping          = Ratio;
            return true;
        }
        else
        {
        	return false;
        }
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

bool FPhysics2DSystem::SetJointTarget(FScene& Scene, FEntity Entity, float Target)
{
	if (!std::isfinite(Target))
	{
		return false;
	}
	const bool bAny = EditJointComponents(Scene, Entity, [&]<typename TJoint>(TJoint& Joint) {
		if constexpr (std::is_same_v<TJoint, FRevoluteJoint2DComponent>)
		{
			Joint.TargetAngle = std::clamp(Target, -180.0f, 180.0f);
			return true;
		}
		else if constexpr (std::is_same_v<TJoint, FPrismaticJoint2DComponent>)
		{
			Joint.TargetTranslation = Target;
			return true;
		}
		else
		{
			return false;
		}
	});
	ApplyJointLiveSettings(Scene, Entity);
	return bAny;
}

const FPhysics2DSystem::FJointState* FPhysics2DSystem::FindLiveJoint(FEntity Entity, uint8* OutKind) const
{
	if (!World)
	{
		return nullptr;
	}
	for (const EJoint2DKind Kind : { EJoint2DKind::Revolute, EJoint2DKind::Prismatic, EJoint2DKind::Wheel, EJoint2DKind::Distance, EJoint2DKind::Weld })
	{
		const auto Found = Joints.find(FJointKey{ Entity, static_cast<uint8>(Kind) });
		if (Found != Joints.end() && Found->second.Joint != FPhysics2DWorld::InvalidJoint && World->IsJointAlive(Found->second.Joint))
		{
			if (OutKind != nullptr)
			{
				*OutKind = static_cast<uint8>(Kind);
			}
			return &Found->second;
		}
	}
	return nullptr;
}

float FPhysics2DSystem::GetJointAngle(FEntity Entity) const
{
	const FJointState*           Joint = FindLiveJoint(Entity);
	FPhysics2DWorld::FJointState State;
	return Joint != nullptr && World->GetJointState(Joint->Joint, State) ? State.Angle * FMath::RadToDeg : 0.0f;
}

float FPhysics2DSystem::GetJointTranslation(FEntity Entity) const
{
	uint8                        Kind  = 0;
	const FJointState*           Joint = FindLiveJoint(Entity, &Kind);
	FPhysics2DWorld::FJointState State;
	if (Joint == nullptr || !World->GetJointState(Joint->Joint, State))
	{
		return 0.0f;
	}
	return Kind == static_cast<uint8>(EJoint2DKind::Distance) ? State.Length : State.Translation;
}

float FPhysics2DSystem::GetJointSpeed(FEntity Entity) const
{
	uint8                        Kind  = 0;
	const FJointState*           Joint = FindLiveJoint(Entity, &Kind);
	FPhysics2DWorld::FJointState State;
	if (Joint == nullptr || !World->GetJointState(Joint->Joint, State))
	{
		return 0.0f;
	}
	if (Kind == static_cast<uint8>(EJoint2DKind::Prismatic))
	{
		return State.LinearSpeed;
	}
	return Kind == static_cast<uint8>(EJoint2DKind::Distance) ? 0.0f : State.AngularSpeed * FMath::RadToDeg;
}

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

	void AppendCommon(std::vector<float>& Out, const FVector2& Anchor, float BreakForce, bool bCollideConnected)
	{
		Out.insert(Out.end(), { Anchor.X, Anchor.Y, BreakForce, bCollideConnected ? 1.0f : 0.0f });
	}

	std::vector<float> MakeSignature(const FDistanceJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.TargetAnchor.X, Joint.TargetAnchor.Y, Joint.Length, Joint.MinLength, Joint.MaxLength, Joint.SpringFrequency,
		                        Joint.SpringDamping });
		return Out;
	}
	std::vector<float> MakeSignature(const FRevoluteJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.bLimit ? 1.0f : 0.0f, Joint.LowerAngle, Joint.UpperAngle, Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed,
		                        Joint.MaxMotorTorque });
		return Out;
	}
	std::vector<float> MakeSignature(const FPrismaticJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.Axis.X, Joint.Axis.Y, Joint.bLimit ? 1.0f : 0.0f, Joint.LowerTranslation, Joint.UpperTranslation,
		                        Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed, Joint.MaxMotorForce });
		return Out;
	}
	std::vector<float> MakeSignature(const FWeldJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.LinearFrequency, Joint.AngularFrequency, Joint.Damping });
		return Out;
	}
	std::vector<float> MakeSignature(const FWheelJoint2DComponent& Joint)
	{
		std::vector<float> Out;
		AppendCommon(Out, Joint.Anchor, Joint.BreakForce, Joint.bCollideConnected);
		Out.insert(Out.end(), { Joint.Axis.X, Joint.Axis.Y, Joint.SpringFrequency, Joint.SpringDamping, Joint.bLimit ? 1.0f : 0.0f,
		                        Joint.LowerTranslation, Joint.UpperTranslation, Joint.bMotor ? 1.0f : 0.0f, Joint.MotorSpeed, Joint.MaxMotorTorque });
		return Out;
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
			State.Signature  = std::move(Signature);
			State.BreakForce = Joint.BreakForce;
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

// 2D 관절 (FPhysics2DSystem::SyncJoints — Physics/Physics2DSystem.h "관절"): 진자 주기, 각/이동 한계, 모터, 스프링·용접·바퀴,
// 끊어짐 이벤트, 바디 삭제/재생성 순서, 마우스 끌기
#include "Core/Testing/TestFramework.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Physics/Physics2DSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <vector>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	FEntity AddBox(FScene& Scene, const char* Name, const FVector3& Position, const FVector2& Size, bool bDynamic = true)
	{
		const FEntity Entity                 = Scene.CreateEntity(Name);
		Scene.GetTransform(Entity).Position = Position;
		Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Entity).Size = Size;
		if (bDynamic)
		{
			Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Entity).AngularDamping = 0.0f;
		}
		return Entity;
	}

	FEntity AddBall(FScene& Scene, const char* Name, const FVector3& Position, float Radius)
	{
		const FEntity Entity                 = Scene.CreateEntity(Name);
		Scene.GetTransform(Entity).Position = Position;
		Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Entity).Radius = Radius;
		Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Entity).AngularDamping = 0.0f;
		return Entity;
	}

	void Step(FPhysics2DSystem& Physics, FScene& Scene, int32 Frames)
	{
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
	}

	float PlaneAngleDegrees(const FScene& Scene, FEntity Entity)
	{
		return Physics2DMath::AngleFromRotation(Scene.GetTransform(Entity).Rotation) * FMath::RadToDeg;
	}

	FPhysics2DSystem& BeginPhysics(FPhysics2DSystem& Physics)
	{
		Physics.Begin();
		Physics.SetInterpolation(false);
		return Physics;
	}
} // namespace

// 회전 관절로 월드에 매단 작은 공 = 단진자: 주기 ≈ 2π√(L/g) (작은 진폭 10°, 3% 안)
E_TEST(Physics2DJoint_RevolutePendulumPeriod)
{
	constexpr float Length = 100.0f;
	constexpr float Theta  = 10.0f * FMath::DegToRad;
	FScene          Scene;
	const FEntity   Bob = AddBall(Scene, "Bob", FVector3(Length * std::sin(Theta), 0.0f, -Length * std::cos(Theta)), 3.0f);
	FRevoluteJoint2DComponent& Joint = Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Bob);
	Joint.Anchor = FVector2(-Length * std::sin(Theta), Length * std::cos(Theta)); // 월드 원점 = 축
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	Physics.Update(Scene, 0.0f);
	E_EXPECT_TRUE(Physics.HasJoint(Bob));
	E_EXPECT_EQ(Physics.GetJointCount(), 1u);
	const float Gravity  = std::abs(Physics.GetWorld()->GetGravity().Y);
	const float Expected = FMath::TwoPi * std::sqrt(Length / Gravity);

	std::vector<float> Crossings; // X가 0을 지나는 시각 (반주기 간격)
	float              PreviousX = Scene.GetTransform(Bob).Position.X;
	for (int32 Index = 1; Index <= 480; ++Index)
	{
		Step(Physics, Scene, 1);
		const float X = Scene.GetTransform(Bob).Position.X;
		if ((PreviousX > 0.0f) != (X > 0.0f))
		{
			const float Fraction = PreviousX / (PreviousX - X);
			Crossings.push_back((static_cast<float>(Index - 1) + Fraction) * Frame);
		}
		PreviousX = X;
		// 줄 길이 유지
		const FVector3 Position = Scene.GetTransform(Bob).Position;
		E_EXPECT_NEAR(std::sqrt(Position.X * Position.X + Position.Z * Position.Z), Length, 0.5f);
	}
	E_EXPECT_TRUE(Crossings.size() >= 5u);
	if (Crossings.size() >= 5u)
	{
		const float Measured = 2.0f * (Crossings[4] - Crossings[0]) / 4.0f;
		E_EXPECT_NEAR(Measured, Expected, Expected * 0.03f);
	}
	Physics.End();
}

// 각 한계: 수평 막대가 중력으로 떨어지다 -30°에서 멈춘다. 모터: 월드 축 바퀴가 지정 속도로 돈다
E_TEST(Physics2DJoint_RevoluteLimitAndMotor)
{
	FScene        Scene;
	const FEntity Bar = AddBox(Scene, "Bar", FVector3(100.0f, 0.0f, 0.0f), FVector2(200.0f, 10.0f));
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Bar).AngularDamping = 2.0f;
	FRevoluteJoint2DComponent& Hinge = Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Bar);
	Hinge.Anchor     = FVector2(-100.0f, 0.0f);
	Hinge.bLimit     = true;
	Hinge.LowerAngle = -30.0f;
	Hinge.UpperAngle = 30.0f;

	const FEntity Wheel = AddBall(Scene, "Wheel", FVector3(0.0f, 0.0f, 500.0f), 50.0f);
	FRevoluteJoint2DComponent& Motor = Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Wheel);
	Motor.bMotor         = true;
	Motor.MotorSpeed     = 180.0f;
	Motor.MaxMotorTorque = 10000.0f;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	Step(Physics, Scene, 180);
	E_EXPECT_NEAR(PlaneAngleDegrees(Scene, Bar), -30.0f, 2.0f);
	E_EXPECT_NEAR(Physics.GetAngularVelocity(Wheel), FMath::Pi, 0.05f);
	E_EXPECT_NEAR(Scene.GetTransform(Wheel).Position.Z, 500.0f, 0.5f); // 축에 매달려 떨어지지 않음
	Physics.End();
}

// 미닫이: X축으로만 움직이고(중력에도 Z 유지) 모터로 밀려 위 한계(+50cm)에서 멈춘다
E_TEST(Physics2DJoint_PrismaticAxisLimitMotor)
{
	FScene        Scene;
	const FEntity Slider = AddBox(Scene, "Slider", FVector3(0.0f, 0.0f, 200.0f), FVector2(40.0f, 40.0f));
	FPrismaticJoint2DComponent& Joint = Scene.GetRegistry().Emplace<FPrismaticJoint2DComponent>(Slider);
	Joint.Axis             = FVector2(1.0f, 0.0f);
	Joint.bLimit           = true;
	Joint.LowerTranslation = -20.0f;
	Joint.UpperTranslation = 50.0f;
	Joint.bMotor           = true;
	Joint.MotorSpeed       = 100.0f;
	Joint.MaxMotorForce    = 10000.0f;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	Step(Physics, Scene, 15); // 0.25초 ≈ 25cm
	E_EXPECT_NEAR(Scene.GetTransform(Slider).Position.X, 25.0f, 3.0f);
	Step(Physics, Scene, 105);
	E_EXPECT_NEAR(Scene.GetTransform(Slider).Position.X, 50.0f, 1.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Slider).Position.Z, 200.0f, 0.5f);
	E_EXPECT_NEAR(PlaneAngleDegrees(Scene, Slider), 0.0f, 0.5f);
	Physics.End();
}

// 거리(딱딱함): 두 동적 바디 사이 거리 유지. 거리(스프링): 늘어났다 돌아와 정지 길이 + 처짐 근처. 용접: 월드에 붙은 상자가 움직이지 않음.
// 바퀴: 정적 차체에 매단 바퀴가 서스펜션 위에서 모터 속도로 돈다
E_TEST(Physics2DJoint_DistanceWeldWheel)
{
	FScene        Scene;
	const FEntity Anchor = AddBox(Scene, "Anchor", FVector3(0.0f, 0.0f, 300.0f), FVector2(20.0f, 20.0f), false);
	const FEntity Rod    = AddBall(Scene, "Rod", FVector3(80.0f, 0.0f, 300.0f), 5.0f);
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Rod).LinearDamping = 3.0f; // 흔들림이 가라앉아 아래에 매달린다
	FDistanceJoint2DComponent& Rigid = Scene.GetRegistry().Emplace<FDistanceJoint2DComponent>(Rod);
	Rigid.Target = Anchor; // 길이 = 처음 거리 80

	const FEntity Spring = AddBall(Scene, "Spring", FVector3(500.0f, 0.0f, 200.0f), 5.0f);
	FDistanceJoint2DComponent& Soft = Scene.GetRegistry().Emplace<FDistanceJoint2DComponent>(Spring);
	Soft.TargetAnchor    = FVector2(500.0f, 300.0f); // 월드 평면 위치
	Soft.SpringFrequency = 2.0f;
	Soft.SpringDamping   = 1.0f;

	const FEntity Welded = AddBox(Scene, "Welded", FVector3(-300.0f, 0.0f, 100.0f), FVector2(50.0f, 50.0f));
	Scene.GetRegistry().Emplace<FWeldJoint2DComponent>(Welded);

	const FEntity Chassis = AddBox(Scene, "Chassis", FVector3(-600.0f, 0.0f, 300.0f), FVector2(200.0f, 20.0f), false);
	const FEntity Wheel   = AddBall(Scene, "Wheel", FVector3(-600.0f, 0.0f, 250.0f), 25.0f);
	FWheelJoint2DComponent& Axle = Scene.GetRegistry().Emplace<FWheelJoint2DComponent>(Wheel);
	Axle.Target         = Chassis;
	Axle.bMotor         = true;
	Axle.MotorSpeed     = -360.0f;
	Axle.MaxMotorTorque = 10000.0f;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	Step(Physics, Scene, 240);
	const FVector3 RodPosition = Scene.GetTransform(Rod).Position;
	E_EXPECT_NEAR(std::sqrt(RodPosition.X * RodPosition.X + (RodPosition.Z - 300.0f) * (RodPosition.Z - 300.0f)), 80.0f, 0.5f);
	E_EXPECT_TRUE(RodPosition.Z < 300.0f - 70.0f); // 아래로 내려와 매달렸다

	// 스프링: 정지 길이 100 + 처짐 g/ω² ≈ 980/(2π·2)² ≈ 6.2cm
	const float Gravity = std::abs(Physics.GetWorld()->GetGravity().Y);
	const float Omega   = FMath::TwoPi * 2.0f;
	E_EXPECT_NEAR(Scene.GetTransform(Spring).Position.Z, 300.0f - 100.0f - Gravity / (Omega * Omega), 2.0f);

	E_EXPECT_NEAR(Scene.GetTransform(Welded).Position.Z, 100.0f, 0.5f);
	E_EXPECT_NEAR(Scene.GetTransform(Welded).Position.X, -300.0f, 0.5f);

	E_EXPECT_NEAR(Physics.GetAngularVelocity(Wheel), -FMath::TwoPi, 0.1f);
	const float Sag = 250.0f - Scene.GetTransform(Wheel).Position.Z; // 서스펜션 4Hz 처짐 ≈ g/(2π·4)² ≈ 1.6cm
	E_EXPECT_TRUE(Sag > 0.0f && Sag < 5.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Wheel).Position.X, -600.0f, 0.5f);
	E_EXPECT_EQ(Physics.GetJointCount(), 4u);
	Physics.End();
}

// 끊어짐: 무거운 상자를 매단 관절의 BreakForce가 무게보다 작으면 첫 스텝에 끊기고 JointBreak(Self = 관절 엔티티, Other = 대상, 힘 N)
// → 떨어진다. 끊긴 관절은 컴포넌트를 다시 달 때까지 그대로
E_TEST(Physics2DJoint_BreakEvent)
{
	FScene        Scene;
	const FEntity Hook  = AddBox(Scene, "Hook", FVector3(0.0f, 0.0f, 500.0f), FVector2(20.0f, 20.0f), false);
	const FEntity Crate = AddBox(Scene, "Crate", FVector3(0.0f, 0.0f, 400.0f), FVector2(50.0f, 50.0f));
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Crate).Mass = 10.0f; // 무게 ≈ 98 N
	FRevoluteJoint2DComponent& Joint = Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Crate);
	Joint.Target     = Hook;
	Joint.Anchor     = FVector2(0.0f, 100.0f);
	Joint.BreakForce = 50.0f;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	bool  bBroke = false;
	float Force  = 0.0f;
	for (int32 Index = 0; Index < 30 && !bBroke; ++Index)
	{
		Step(Physics, Scene, 1);
		for (const FCollisionEvent& Event : Physics.GetCollisionEvents())
		{
			if (Event.Type == ECollisionEventType::JointBreak)
			{
				bBroke = true;
				Force  = Event.Impulse;
				E_EXPECT_TRUE(Event.Self == Crate);
				E_EXPECT_TRUE(Event.Other == Hook);
			}
		}
	}
	E_EXPECT_TRUE(bBroke);
	E_EXPECT_TRUE(Force > 50.0f);
	E_EXPECT_TRUE(Physics.IsJointBroken(Crate));
	E_EXPECT_FALSE(Physics.HasJoint(Crate));
	E_EXPECT_EQ(Physics.GetJointCount(), 0u);
	Step(Physics, Scene, 30);
	E_EXPECT_TRUE(Scene.GetTransform(Crate).Position.Z < 300.0f);
	E_EXPECT_EQ(Physics.GetJointCount(), 0u); // 끊긴 채로

	// 컴포넌트를 지웠다 다시 달면 (BreakForce 0 = 안 끊김) 지금 자세로 다시 만든다
	Scene.GetRegistry().Remove<FRevoluteJoint2DComponent>(Crate);
	Step(Physics, Scene, 1);
	FRevoluteJoint2DComponent& Again = Scene.GetRegistry().Emplace<FRevoluteJoint2DComponent>(Crate);
	Again.Target                     = Hook;
	Step(Physics, Scene, 1);
	E_EXPECT_TRUE(Physics.HasJoint(Crate));
	E_EXPECT_FALSE(Physics.IsJointBroken(Crate));
	Physics.End();
}

// 바디 삭제/재생성 순서: 바디를 지우면 그 관절이 먼저 사라지고(월드 수준), 시스템은 대상 바디가 사라지면 월드 고정으로,
// 자기 바디를 다시 만들면 관절도 다시 만든다. 대상 엔티티 파괴·자기 엔티티 파괴도 안전
E_TEST(Physics2DJoint_BodyDeletionOrder)
{
	{
		FPhysics2DWorld    World;
		FPhysics2DBodyDesc Body;
		Body.Shapes.push_back({});
		const uint32 A = World.CreateBody(Body);
		const uint32 B = World.CreateBody(Body);
		FPhysics2DJointDesc Joint;
		Joint.Body1 = A;
		Joint.Body2 = B;
		const uint32 Handle = World.CreateJoint(Joint);
		E_EXPECT_TRUE(World.IsJointAlive(Handle));
		World.DestroyBody(A);
		E_EXPECT_FALSE(World.IsJointAlive(Handle));
		E_EXPECT_EQ(World.GetJointCount(), 0u);
		World.DestroyJoint(Handle); // 이미 없음 — 무시
		Joint.Body1 = A;            // 사라진 대상 = 실패
		E_EXPECT_EQ(World.CreateJoint(Joint), FPhysics2DWorld::InvalidJoint);
		World.Step(Frame);
	}

	FScene        Scene;
	const FEntity Target = AddBox(Scene, "Target", FVector3(0.0f, 0.0f, 300.0f), FVector2(20.0f, 20.0f));
	const FEntity Self   = AddBox(Scene, "Self", FVector3(100.0f, 0.0f, 300.0f), FVector2(20.0f, 20.0f));
	Scene.GetRegistry().Emplace<FWeldJoint2DComponent>(Self).Target = Target;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	Step(Physics, Scene, 30);
	E_EXPECT_TRUE(Physics.HasJoint(Self));
	E_EXPECT_NEAR(Scene.GetTransform(Self).Position.X - Scene.GetTransform(Target).Position.X, 100.0f, 0.5f); // 함께 떨어진다

	// 자기 바디 다시 만들기 (모양 변경) → 관절도 다시
	Scene.GetRegistry().Get<FBoxCollider2DComponent>(Self).Size = FVector2(30.0f, 30.0f);
	Step(Physics, Scene, 1);
	E_EXPECT_TRUE(Physics.HasJoint(Self));
	E_EXPECT_EQ(Physics.GetJointCount(), 1u);

	// 대상 바디 사라짐 → 그 자리 월드 고정 (Self가 더 떨어지지 않는다)
	Scene.GetRegistry().Remove<FBoxCollider2DComponent>(Target);
	Scene.GetRegistry().Remove<FRigidBody2DComponent>(Target);
	Step(Physics, Scene, 1);
	E_EXPECT_TRUE(Physics.HasJoint(Self));
	const float HeldZ = Scene.GetTransform(Self).Position.Z;
	Step(Physics, Scene, 30);
	E_EXPECT_NEAR(Scene.GetTransform(Self).Position.Z, HeldZ, 0.5f);

	// 대상 엔티티 파괴 → 계속 월드 고정, 자기 엔티티 파괴 → 관절 정리
	Scene.DestroyEntity(Target);
	Step(Physics, Scene, 2);
	E_EXPECT_EQ(Physics.GetJointCount(), 1u);
	Scene.DestroyEntity(Self);
	Step(Physics, Scene, 2);
	E_EXPECT_EQ(Physics.GetJointCount(), 0u);
	E_EXPECT_EQ(Physics.GetBodyCount(), 0u);
	Physics.End();
}

// 마우스 끌기: 동적 상자를 잡아 목표로 끌면 따라오고, 놓으면 떨어진다. 정적 바디는 잡을 수 없다
E_TEST(Physics2DJoint_MouseDrag)
{
	FScene        Scene;
	const FEntity Box    = AddBox(Scene, "Box", FVector3(0.0f, 0.0f, 0.0f), FVector2(40.0f, 40.0f));
	const FEntity Ground = AddBox(Scene, "Ground", FVector3(0.0f, 0.0f, -100.0f), FVector2(1000.0f, 20.0f), false);
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	BeginPhysics(Physics);
	Physics.Update(Scene, 0.0f);
	E_EXPECT_FALSE(Physics.BeginDrag(Ground, FVector2(0.0f, -100.0f)));
	E_EXPECT_TRUE(Physics.BeginDrag(Box, FVector2(0.0f, 0.0f)));
	E_EXPECT_TRUE(Physics.IsDragging(Box));
	E_EXPECT_TRUE(Physics.UpdateDrag(Box, FVector2(200.0f, 150.0f)));
	Step(Physics, Scene, 120);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.X, 200.0f, 10.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 150.0f, 10.0f);
	E_EXPECT_TRUE(Physics.EndDrag(Box));
	E_EXPECT_FALSE(Physics.IsDragging(Box));
	E_EXPECT_FALSE(Physics.UpdateDrag(Box, FVector2()));
	Step(Physics, Scene, 60);
	E_EXPECT_TRUE(Scene.GetTransform(Box).Position.Z < 0.0f);

	// 바디를 다시 만들면 끌기는 끝난다
	E_EXPECT_TRUE(Physics.BeginDrag(Box, Physics2DMath::ToPlane(Scene.GetTransform(Box).Position)));
	Scene.GetRegistry().Get<FBoxCollider2DComponent>(Box).Size = FVector2(30.0f, 30.0f);
	Step(Physics, Scene, 1);
	E_EXPECT_FALSE(Physics.IsDragging(Box));
	Physics.End();
}

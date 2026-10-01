// 물리 관절 (Physics/PhysicsSystem.h "관절"): 경첩 제한/모터, 거리(줄), 구 관절 사슬, 끊어짐, 이은 바디 충돌 끄기, 바디 재생성 뒤 다시 잇기
#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsReflection.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	FEntity AddBox(FScene& Scene, const char* Name, const FVector3& Position, const FVector3& HalfExtents, float Mass)
	{
		const FEntity Box = Scene.CreateEntity(Name);
		Scene.GetTransform(Box).Position = Position;
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Box).HalfExtents = HalfExtents;
		FRigidBodyComponent& Body = Scene.GetRegistry().Emplace<FRigidBodyComponent>(Box);
		Body.Mass                 = Mass;
		return Box;
	}

	uint32 Simulate(FPhysicsSystem& Physics, FScene& Scene, float Seconds, int32* OutBreaks = nullptr)
	{
		const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
		uint32      Steps  = 0;
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Steps += Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
			if (OutBreaks != nullptr)
			{
				for (const FCollisionEvent& Event : Physics.GetCollisionEvents())
				{
					*OutBreaks += Event.Type == ECollisionEventType::JointBreak ? 1 : 0;
				}
			}
		}
		return Steps;
	}

	float YawDegrees(const FQuat& Rotation)
	{
		const FVector3 Forward = Rotation.GetForwardVector();
		return FMath::RadiansToDegrees(std::atan2(Forward.Y, Forward.X));
	}
} // namespace

E_TEST(PhysicsJoints_RegisteredForReflection)
{
	RegisterPhysicsTypes();
	for (const char* Name : { "FixedJointComponent", "HingeJointComponent", "DistanceJointComponent", "BallJointComponent" })
	{
		E_EXPECT_TRUE(FTypeRegistry::Get().Find(Name) != nullptr);
	}
}

// 경첩 문: 월드(문틀)에 Z축 경첩, 0~90도 제한 — 세게 돌려도 90도에서 멈추고 경첩 지점이 제자리
E_TEST(PhysicsJoints_HingeDoorStaysWithinLimit)
{
	FScene        Scene;
	const FEntity Door = AddBox(Scene, "Door", FVector3(50.0f, 0.0f, 100.0f), FVector3(50.0f, 5.0f, 100.0f), 20.0f);
	Scene.GetRegistry().Get<FRigidBodyComponent>(Door).bUseGravity = false;
	FHingeJointComponent& Hinge = Scene.GetRegistry().Emplace<FHingeJointComponent>(Door);
	Hinge.Anchor                = FVector3(-50.0f, 0.0f, 0.0f); // 왼쪽 모서리
	Hinge.Axis                  = FVector3(0.0f, 0.0f, 1.0f);
	Hinge.bLimit                = true;
	Hinge.MinAngle              = 0.0f;
	Hinge.MaxAngle              = 90.0f;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	E_EXPECT_TRUE(Physics.HasJoint(Door));
	E_EXPECT_EQ(Physics.GetJointCount(), 1u);
	FPhysicsBodyMotion Motion;
	E_EXPECT_TRUE(Physics.GetBodyMotion(Door, Motion));
	Motion.AngularVelocity = FVector3(0.0f, 0.0f, 6.0f); // rad/s, +Z = 왼손 좌표계에서 +X → +Y 쪽
	Motion.LinearVelocity  = FVector3(0.0f, 300.0f, 0.0f);
	Physics.SetBodyMotion(Door, Motion);
	Simulate(Physics, Scene, 1.0f);
	const float Yaw = YawDegrees(Scene.GetTransform(Door).Rotation);
	E_EXPECT_TRUE(Yaw > 60.0f && Yaw < 93.0f);
	const FVector3 HingeWorld = Scene.GetTransform(Door).WorldMatrix.TransformPosition(FVector3(-50.0f, 0.0f, 0.0f));
	E_EXPECT_EQUALS(HingeWorld, FVector3(0.0f, 0.0f, 100.0f), 1.5f);
	Physics.End();
}

// 경첩 모터: 바퀴가 목표 각속도로 돈다
E_TEST(PhysicsJoints_HingeMotorSpins)
{
	FScene        Scene;
	const FEntity Wheel = AddBox(Scene, "Wheel", FVector3(0.0f, 0.0f, 200.0f), FVector3(40.0f, 5.0f, 40.0f), 5.0f);
	FHingeJointComponent& Hinge = Scene.GetRegistry().Emplace<FHingeJointComponent>(Wheel);
	Hinge.Axis                  = FVector3(0.0f, 1.0f, 0.0f);
	Hinge.bMotor                = true;
	Hinge.MotorSpeed            = 180.0f;
	Hinge.MotorMaxTorque        = 500.0f;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 1.0f);
	FPhysicsBodyMotion Motion;
	E_EXPECT_TRUE(Physics.GetBodyMotion(Wheel, Motion));
	E_EXPECT_NEAR(std::abs(Motion.AngularVelocity.Y), FMath::Pi, 0.2f);
	E_EXPECT_NEAR(Scene.GetTransform(Wheel).Position.Z, 200.0f, 1.5f); // 중심이 축에 매달려 있다
	Physics.End();
}

// 거리 관절(줄): 월드 점 아래 100cm에 매단 추는 늘어나지 않는다. 스프링이면 늘어났다가 돌아온다
E_TEST(PhysicsJoints_DistanceRopeAndSpring)
{
	FScene        Scene;
	const FEntity Weight = AddBox(Scene, "Weight", FVector3(60.0f, 0.0f, 300.0f), FVector3(10.0f, 10.0f, 10.0f), 2.0f);
	FDistanceJointComponent& Rope = Scene.GetRegistry().Emplace<FDistanceJointComponent>(Weight);
	Rope.TargetAnchor             = FVector3(0.0f, 0.0f, 380.0f); // 월드 점 (처음 거리 100cm)
	Rope.MinDistance              = 0.0f;
	const FEntity Spring          = AddBox(Scene, "Spring", FVector3(500.0f, 0.0f, 300.0f), FVector3(10.0f, 10.0f, 10.0f), 2.0f);
	FDistanceJointComponent& Coil = Scene.GetRegistry().Emplace<FDistanceJointComponent>(Spring);
	Coil.TargetAnchor             = FVector3(500.0f, 0.0f, 400.0f);
	Coil.SpringFrequency          = 1.0f;
	Coil.SpringDamping            = 0.2f;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	float MaxSpringStretch = 0.0f;
	float MinWeightX       = 1.0e9f;
	float MaxRopeLength    = 0.0f;
	for (int32 Index = 0; Index < 120; ++Index)
	{
		Simulate(Physics, Scene, Frame);
		MaxRopeLength    = std::max(MaxRopeLength, FVector3::Distance(Scene.GetTransform(Weight).Position, FVector3(0.0f, 0.0f, 380.0f)));
		MinWeightX       = std::min(MinWeightX, Scene.GetTransform(Weight).Position.X);
		MaxSpringStretch = std::max(MaxSpringStretch, FVector3::Distance(Scene.GetTransform(Spring).Position, FVector3(500.0f, 0.0f, 400.0f)) - 100.0f);
	}
	E_EXPECT_TRUE(MaxRopeLength < 102.0f); // 줄은 늘어나지 않는다
	E_EXPECT_TRUE(MinWeightX < 0.0f);      // 진자처럼 반대편까지 흔들렸다
	E_EXPECT_TRUE(MaxSpringStretch > 5.0f);                       // 스프링은 늘어난다 (딱딱한 줄이 아님)
	Physics.End();
}

// 구 관절 사슬: 월드에 매단 고리 3개가 늘어지지만 끊어지지 않는다 (맞닿은 고리끼리는 충돌을 끈다)
E_TEST(PhysicsJoints_BallChainHangs)
{
	FScene  Scene;
	FEntity Previous;
	FEntity Links[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		// 가로로 눕혀 시작 → 중력에 아래로 늘어진다. 고리 길이 40cm, 이음점은 고리 끝
		Links[Index] = AddBox(Scene, "Link", FVector3(20.0f + 40.0f * static_cast<float>(Index), 0.0f, 500.0f), FVector3(22.0f, 4.0f, 4.0f), 1.0f);
		FBallJointComponent& Ball = Scene.GetRegistry().Emplace<FBallJointComponent>(Links[Index]);
		Ball.Anchor               = FVector3(-20.0f, 0.0f, 0.0f);
		Ball.Target               = Previous;
		Previous                  = Links[Index];
	}
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	float LowestTip = 1.0e9f;
	float FarthestTip = 0.0f;
	for (int32 Index = 0; Index < 180; ++Index) // 3초: 감쇠가 작아 진자처럼 계속 흔들린다
	{
		Simulate(Physics, Scene, Frame);
		const FVector3 Tip = Scene.GetTransform(Links[2]).WorldMatrix.TransformPosition(FVector3(20.0f, 0.0f, 0.0f));
		LowestTip          = std::min(LowestTip, Tip.Z);
		FarthestTip        = std::max(FarthestTip, FVector3::Distance(Tip, FVector3(0.0f, 0.0f, 500.0f)));
	}
	E_EXPECT_EQ(Physics.GetJointCount(), 3u);
	E_EXPECT_TRUE(LowestTip < 390.0f);   // 아래로 늘어진다
	E_EXPECT_TRUE(FarthestTip < 126.0f); // 사슬 길이(120) 안 — 이음점이 벌어지지 않는다
	Physics.End();
}

// 끊어지는 힘: 무거운 상자(98N)는 50N 고정 관절을 끊고 떨어지고, 가벼운 상자(9.8N)는 매달려 있다
E_TEST(PhysicsJoints_BreakForce)
{
	FScene        Scene;
	const FEntity Heavy = AddBox(Scene, "Heavy", FVector3(0.0f, 0.0f, 300.0f), FVector3(10.0f, 10.0f, 10.0f), 10.0f);
	Scene.GetRegistry().Emplace<FFixedJointComponent>(Heavy).BreakForce = 50.0f;
	const FEntity Light = AddBox(Scene, "Light", FVector3(200.0f, 0.0f, 300.0f), FVector3(10.0f, 10.0f, 10.0f), 1.0f);
	Scene.GetRegistry().Emplace<FFixedJointComponent>(Light).BreakForce = 50.0f;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	int32 Breaks = 0;
	Simulate(Physics, Scene, 1.0f, &Breaks);
	E_EXPECT_EQ(Breaks, 1);
	E_EXPECT_TRUE(Physics.IsJointBroken(Heavy));
	E_EXPECT_FALSE(Physics.IsJointBroken(Light));
	E_EXPECT_TRUE(Scene.GetTransform(Heavy).Position.Z < 200.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Light).Position.Z, 300.0f, 1.0f);
	E_EXPECT_EQ(Physics.GetJointCount(), 1u);

	// 끊긴 관절은 그대로 (설정을 바꿔도 다시 붙지 않는다), 컴포넌트를 지우면 기록도 사라진다
	Scene.GetRegistry().Get<FFixedJointComponent>(Heavy).BreakForce = 0.0f;
	Simulate(Physics, Scene, 0.1f);
	E_EXPECT_TRUE(Physics.IsJointBroken(Heavy));
	Scene.GetRegistry().Remove<FFixedJointComponent>(Heavy);
	Simulate(Physics, Scene, 0.1f);
	E_EXPECT_FALSE(Physics.IsJointBroken(Heavy));
	Physics.End();
}

// 겹쳐 놓은 두 상자를 고정 관절로 이으면(서로 충돌 끔) 튕겨 나가지 않는다. 콜라이더를 바꿔 바디를 다시 만들어도 관절이 다시 이어진다
E_TEST(PhysicsJoints_ConnectedCollisionDisabledAndRecreated)
{
	FScene        Scene;
	const FEntity Base  = AddBox(Scene, "Base", FVector3(0.0f, 0.0f, 300.0f), FVector3(30.0f, 30.0f, 30.0f), 5.0f);
	Scene.GetRegistry().Get<FRigidBodyComponent>(Base).bUseGravity = false;
	Scene.GetRegistry().Emplace<FFixedJointComponent>(Base); // 월드에 고정
	const FEntity Part = AddBox(Scene, "Part", FVector3(20.0f, 0.0f, 300.0f), FVector3(30.0f, 30.0f, 30.0f), 5.0f); // 크게 겹침
	Scene.GetRegistry().Emplace<FFixedJointComponent>(Part).Target = Base;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 1.0f);
	E_EXPECT_EQ(Physics.GetJointCount(), 2u);
	E_EXPECT_EQUALS(Scene.GetTransform(Part).Position, FVector3(20.0f, 0.0f, 300.0f), 2.0f);
	E_EXPECT_TRUE(Physics.GetVelocity(Part).Length() < 10.0f);

	Scene.GetRegistry().Get<FBoxColliderComponent>(Part).HalfExtents = FVector3(25.0f, 25.0f, 25.0f); // 바디 재생성
	Simulate(Physics, Scene, 0.5f);
	E_EXPECT_EQ(Physics.GetJointCount(), 2u);
	E_EXPECT_EQUALS(Scene.GetTransform(Part).Position, FVector3(20.0f, 0.0f, 300.0f), 3.0f);
	Physics.End();
}

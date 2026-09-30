#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsMath.h"
#include "Physics/PhysicsReflection.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#pragma warning(push, 0)
#include <Jolt/Jolt.h>
#pragma warning(pop)

#include <cmath>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	FEntity AddFloor(FScene& Scene, float Restitution = 0.0f, float Friction = 0.5f)
	{
		// 윗면이 z = 0인 정적 바닥 (20m x 20m, 두께 20cm)
		const FEntity Floor = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);
		FRigidBodyComponent& Body = Scene.GetRegistry().Emplace<FRigidBodyComponent>(Floor);
		Body.MotionType           = static_cast<int32>(EPhysicsMotionType::Static);
		Body.Restitution          = Restitution;
		Body.Friction             = Friction;
		return Floor;
	}

	FEntity AddDynamicSphere(FScene& Scene, const FVector3& Position, float Radius = 25.0f)
	{
		const FEntity Ball = Scene.CreateEntity("Ball");
		Scene.GetTransform(Ball).Position = Position;
		Scene.GetRegistry().Emplace<FSphereColliderComponent>(Ball).Radius = Radius;
		FRigidBodyComponent& Body = Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ball);
		Body.LinearDamping        = 0.0f;
		Body.AngularDamping       = 0.0f;
		return Ball;
	}

	void Simulate(FPhysicsSystem& Physics, FScene& Scene, float Seconds)
	{
		const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
	}
} // namespace

E_TEST(PhysicsMath_UnitConversionRoundTrip)
{
	const FVector3 Centimeters(123.0f, -45.5f, 1000.0f);
	E_EXPECT_EQUALS(PhysicsMath::ToMeters(Centimeters), FVector3(1.23f, -0.455f, 10.0f), 1.0e-5f);
	E_EXPECT_EQUALS(PhysicsMath::ToCentimeters(PhysicsMath::ToMeters(Centimeters)), Centimeters, 1.0e-3f);
}

E_TEST(PhysicsMath_QuaternionMatchesJolt)
{
	// 엔진 FQuat 성분을 그대로 Jolt Quat로 넘겨도 회전/합성 결과가 같아야 한다 (좌표계 변환 불필요의 근거)
	const FQuat Rotations[] = { FQuat::FromEuler(30.0f, 0.0f, 0.0f), FQuat::FromEuler(0.0f, 45.0f, 0.0f), FQuat::FromEuler(0.0f, 0.0f, -60.0f),
		                        FQuat::FromEuler(20.0f, 110.0f, 35.0f) };
	const FVector3 Vector(1.0f, 2.0f, 3.0f);
	for (const FQuat& Q : Rotations)
	{
		const JPH::Quat JoltQ(Q.X, Q.Y, Q.Z, Q.W);
		const JPH::Vec3 Rotated = JoltQ * JPH::Vec3(Vector.X, Vector.Y, Vector.Z);
		E_EXPECT_EQUALS(Q.RotateVector(Vector), FVector3(Rotated.GetX(), Rotated.GetY(), Rotated.GetZ()), 1.0e-4f);

		const FQuat     Composed      = Q * Rotations[3];
		const JPH::Quat JoltComposed  = JoltQ * JPH::Quat(Rotations[3].X, Rotations[3].Y, Rotations[3].Z, Rotations[3].W);
		E_EXPECT_NEAR(Composed.X, JoltComposed.GetX(), 1.0e-5f);
		E_EXPECT_NEAR(Composed.Y, JoltComposed.GetY(), 1.0e-5f);
		E_EXPECT_NEAR(Composed.Z, JoltComposed.GetZ(), 1.0e-5f);
		E_EXPECT_NEAR(Composed.W, JoltComposed.GetW(), 1.0e-5f);
	}
}

E_TEST(PhysicsMath_WorldToLocal)
{
	const FQuat      ParentRotation = FQuat::FromEuler(0.0f, 90.0f, 0.0f);
	const FMatrix4x4 ParentWorld    = FMatrix4x4::MakeTransform(FVector3(100.0f, 0.0f, 0.0f), ParentRotation, FVector3(2.0f));
	const FVector3   LocalPosition(10.0f, 5.0f, -3.0f);
	const FQuat      LocalRotation = FQuat::FromEuler(15.0f, 0.0f, 0.0f);

	const FMatrix4x4 ChildWorld = FMatrix4x4::MakeTransform(LocalPosition, LocalRotation, FVector3::OneVector) * ParentWorld;
	FVector3         WorldPosition;
	FQuat            WorldRotation;
	FVector3         WorldScale;
	PhysicsMath::DecomposeWorld(ChildWorld, WorldPosition, WorldRotation, WorldScale);

	FVector3 OutPosition;
	FQuat    OutRotation;
	PhysicsMath::WorldToLocal(ParentWorld, WorldPosition, WorldRotation, OutPosition, OutRotation);
	E_EXPECT_EQUALS(OutPosition, LocalPosition, 1.0e-3f);
	E_EXPECT_NEAR(std::abs(FQuat::Dot(OutRotation, LocalRotation)), 1.0f, 1.0e-4f);
}

E_TEST(PhysicsStepper_FixedStepsAndAlpha)
{
	FFixedStepper Stepper;
	E_EXPECT_EQ(Stepper.Advance(1.0f / 60.0f + 1.0e-6f), 1u);
	E_EXPECT_EQ(Stepper.Advance(0.5f / 60.0f), 0u);
	E_EXPECT_NEAR(Stepper.GetAlpha(), 0.5f, 1.0e-3f);
	E_EXPECT_EQ(Stepper.Advance(0.5f / 60.0f), 1u);
	E_EXPECT_EQ(Stepper.Advance(3.0f / 60.0f + 1.0e-4f), 3u);

	// 긴 프레임: 최대 스텝까지만, 초과 시간은 버린다
	Stepper.Reset();
	E_EXPECT_EQ(Stepper.Advance(1.0f), Stepper.MaxSteps);
	E_EXPECT_NEAR(Stepper.GetAlpha(), 0.0f, 1.0e-6f);
	E_EXPECT_EQ(Stepper.Advance(-1.0f), 0u);
}

E_TEST(Physics_FreeFallMatchesGravity)
{
	FScene        Scene;
	const FEntity Ball = AddDynamicSphere(Scene, FVector3(0.0f, 0.0f, 1000.0f));
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 1.0f);
	// z = 1000 - 0.5 g t² ≈ 509.7cm (이산 적분 오차 허용)
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Z, 1000.0f - 0.5f * FUnits::StandardGravity, 15.0f);
	E_EXPECT_NEAR(Physics.GetVelocity(Ball).Z, -FUnits::StandardGravity, 20.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.X, 0.0f, 1.0e-3f);
}

E_TEST(Physics_BoxRestsOnFloor)
{
	FScene Scene;
	AddFloor(Scene);
	const FEntity Box = Scene.CreateEntity("Box");
	Scene.GetTransform(Box).Position = FVector3(0.0f, 0.0f, 150.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Box); // 반 크기 50cm
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Box);
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 3.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 50.0f, 2.0f);
	E_EXPECT_TRUE(Physics.GetVelocity(Box).Length() < 5.0f);
	E_EXPECT_EQ(Physics.GetBodyCount(), 2u);
}

E_TEST(Physics_RaycastHitsFloor)
{
	FScene        Scene;
	const FEntity Floor = AddFloor(Scene);
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);

	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector3(30.0f, -20.0f, 500.0f), FVector3(0.0f, 0.0f, -1.0f), 2000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Floor);
	E_EXPECT_NEAR(Hit.Distance, 500.0f, 0.5f);
	E_EXPECT_EQUALS(Hit.Position, FVector3(30.0f, -20.0f, 0.0f), 0.5f);
	E_EXPECT_EQUALS(Hit.Normal, FVector3::UpVector, 1.0e-3f);

	// 사거리 밖 / 위쪽 방향은 실패
	E_EXPECT_FALSE(Physics.Raycast(FVector3(0.0f, 0.0f, 500.0f), FVector3(0.0f, 0.0f, -1.0f), 100.0f, Hit));
	E_EXPECT_FALSE(Physics.Raycast(FVector3(0.0f, 0.0f, 500.0f), FVector3(0.0f, 0.0f, 1.0f), 2000.0f, Hit));
}

E_TEST(Physics_BodySyncFollowsComponents)
{
	FScene        Scene;
	const FEntity Floor = AddFloor(Scene);
	const FEntity A     = AddDynamicSphere(Scene, FVector3(0.0f, 0.0f, 100.0f));
	const FEntity B     = AddDynamicSphere(Scene, FVector3(200.0f, 0.0f, 100.0f));
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	E_EXPECT_EQ(Physics.GetBodyCount(), 3u);
	E_EXPECT_TRUE(Physics.HasBody(A) && Physics.HasBody(B) && Physics.HasBody(Floor));

	// 콜라이더 제거 → 바디 제거, 엔티티 삭제 → 바디 제거
	Scene.GetRegistry().Remove<FSphereColliderComponent>(A);
	Scene.DestroyEntity(B);
	Physics.Update(Scene, Frame);
	E_EXPECT_EQ(Physics.GetBodyCount(), 1u);
	E_EXPECT_FALSE(Physics.HasBody(A));

	// 모양 변경 → 다시 생성 (수는 그대로)
	Scene.GetRegistry().Get<FBoxColliderComponent>(Floor).HalfExtents = FVector3(500.0f, 500.0f, 10.0f);
	Physics.Update(Scene, Frame);
	E_EXPECT_EQ(Physics.GetBodyCount(), 1u);

	Physics.End();
	E_EXPECT_EQ(Physics.GetBodyCount(), 0u);
	E_EXPECT_FALSE(Physics.IsActive());
}

E_TEST(Physics_RestitutionBounces)
{
	auto MaxHeightAfterBounce = [](float Restitution) {
		FScene        Scene;
		AddFloor(Scene, Restitution);
		const FEntity Ball = AddDynamicSphere(Scene, FVector3(0.0f, 0.0f, 225.0f));
		Scene.GetRegistry().Get<FRigidBodyComponent>(Ball).Restitution = Restitution;
		Scene.UpdateTransforms();

		FPhysicsSystem Physics;
		Physics.Begin();
		Simulate(Physics, Scene, 0.8f); // 낙하 (2m → 약 0.64초) + 충돌
		float MaxHeight = 0.0f;
		for (int32 Index = 0; Index < 60; ++Index)
		{
			Simulate(Physics, Scene, Frame);
			MaxHeight = std::max(MaxHeight, Scene.GetTransform(Ball).Position.Z);
		}
		return MaxHeight;
	};
	const float Bouncy = MaxHeightAfterBounce(1.0f);
	const float Dead   = MaxHeightAfterBounce(0.0f);
	E_EXPECT_TRUE(Bouncy > 100.0f);
	E_EXPECT_TRUE(Dead < 40.0f);
}

E_TEST(Physics_FrictionSlowsSliding)
{
	auto SlideDistance = [](float Friction) {
		FScene        Scene;
		AddFloor(Scene, 0.0f, Friction);
		const FEntity Box = Scene.CreateEntity("Box");
		Scene.GetTransform(Box).Position = FVector3(0.0f, 0.0f, 50.0f);
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Box);
		FRigidBodyComponent& Body = Scene.GetRegistry().Emplace<FRigidBodyComponent>(Box);
		Body.Friction             = Friction;
		Body.LinearDamping        = 0.0f;
		Scene.UpdateTransforms();

		FPhysicsSystem Physics;
		Physics.Begin();
		Physics.Update(Scene, Frame);
		Physics.SetVelocity(Box, FVector3(300.0f, 0.0f, 0.0f));
		Simulate(Physics, Scene, 1.5f);
		return Scene.GetTransform(Box).Position.X;
	};
	const float Slippery = SlideDistance(0.0f);
	const float Rough    = SlideDistance(1.0f);
	E_EXPECT_TRUE(Slippery > 400.0f);
	E_EXPECT_TRUE(Rough < Slippery * 0.5f);
}

E_TEST(Physics_RollingSphereKeepsMomentum)
{
	// 회전 중 스케일 분해 오차로 바디가 다시 생성되면 속도가 0으로 초기화되어 공이 멈춘다
	FScene        Scene;
	AddFloor(Scene);
	const FEntity Ball = AddDynamicSphere(Scene, FVector3(0.0f, 0.0f, 25.0f));
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	Physics.SetVelocity(Ball, FVector3(500.0f, 0.0f, 0.0f));
	Simulate(Physics, Scene, 3.0f);
	// 미끄러짐 → 구름 전환 후 약 5/7 속도(357cm/s)로 계속 굴러간다
	E_EXPECT_TRUE(Physics.GetVelocity(Ball).X > 250.0f);
	E_EXPECT_TRUE(Scene.GetTransform(Ball).Position.X > 800.0f);
}

E_TEST(Physics_ScaleChangeResizesBody)
{
	FScene        Scene;
	const FEntity Ball = AddDynamicSphere(Scene, FVector3(0.0f, 0.0f, 0.0f));
	Scene.GetRegistry().Get<FRigidBodyComponent>(Ball).bUseGravity = false;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector3(0.0f, 0.0f, 500.0f), FVector3(0.0f, 0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_NEAR(Hit.Distance, 475.0f, 0.5f);

	// 실제 스케일 변경은 허용 오차보다 크므로 바디 모양에 반영된다
	Scene.GetTransform(Ball).Scale = FVector3(2.0f, 2.0f, 2.0f);
	Scene.UpdateTransforms();
	Simulate(Physics, Scene, Frame);
	E_EXPECT_TRUE(Physics.Raycast(FVector3(0.0f, 0.0f, 500.0f), FVector3(0.0f, 0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_NEAR(Hit.Distance, 450.0f, 0.5f);
}

E_TEST(Physics_ImpulseAndTeleport)
{
	FScene        Scene;
	const FEntity Ball = AddDynamicSphere(Scene, FVector3(0.0f, 0.0f, 0.0f));
	Scene.GetRegistry().Get<FRigidBodyComponent>(Ball).bUseGravity = false;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	// 질량 1kg, 충격량 100 kg·cm/s → 100cm/s
	Physics.AddImpulse(Ball, FVector3(0.0f, 100.0f, 0.0f));
	Simulate(Physics, Scene, 1.0f);
	E_EXPECT_NEAR(Physics.GetVelocity(Ball).Y, 100.0f, 1.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Y, 100.0f, 5.0f);

	// 트랜스폼을 직접 바꾸면 바디가 순간이동
	Scene.GetTransform(Ball).Position = FVector3(1000.0f, 0.0f, 0.0f);
	Simulate(Physics, Scene, Frame);
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.X, 1000.0f, 1.0f);
}

E_TEST(PhysicsReflection_RegistersComponents)
{
	RegisterPhysicsTypes();
	RegisterPhysicsTypes();
	for (const char* Name : { "RigidBodyComponent", "BoxColliderComponent", "SphereColliderComponent", "CapsuleColliderComponent" })
	{
		E_EXPECT_TRUE(FTypeRegistry::Get().Find(Name) != nullptr);
	}
}

// 2D 물리 (Box2D v3 — Physics/Physics2DSystem.h, 평면 규약 Physics/Physics2DMath.h): 평면·회전 부호, 낙하/정지, 스케일, 원웨이,
// 트리거, 충돌 레이어, 질의, 고정 회전, 키네마틱, 점 목록 파서. 테스트는 Box2D를 직접 부르지 않고 엔진 래퍼만 쓴다
#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DMath.h"
#include "Physics/Physics2DSystem.h"
#include "Scene/CameraProjection.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <vector>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	class FScopedCollisionLayers
	{
	public:
		explicit FScopedCollisionLayers(const FCollisionLayerSettings& Layers) : Saved(FProjectSettings::Get().Collision)
		{
			FProjectSettings::Get().Collision = Layers;
		}
		~FScopedCollisionLayers() { FProjectSettings::Get().Collision = Saved; }

	private:
		FCollisionLayerSettings Saved;
	};

	FEntity AddGround(FScene& Scene, const char* Name = "Ground", float TopZ = 0.0f, float Width = 4000.0f)
	{
		const FEntity Ground = Scene.CreateEntity(Name);
		Scene.GetTransform(Ground).Position = FVector3(0.0f, 0.0f, TopZ - 50.0f);
		Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Ground).Size = FVector2(Width, 100.0f);
		return Ground; // 강체 없음 = 정적
	}

	FEntity AddBox(FScene& Scene, const char* Name, const FVector3& Position, const FVector2& Size = FVector2(100.0f, 100.0f))
	{
		const FEntity Box = Scene.CreateEntity(Name);
		Scene.GetTransform(Box).Position = Position;
		Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Box).Size = Size;
		Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Box);
		return Box;
	}

	FEntity AddBall(FScene& Scene, const char* Name, const FVector3& Position, float Radius = 25.0f)
	{
		const FEntity Ball = Scene.CreateEntity(Name);
		Scene.GetTransform(Ball).Position = Position;
		Scene.GetRegistry().Emplace<FCircleCollider2DComponent>(Ball).Radius = Radius;
		Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Ball);
		return Ball;
	}

	struct FEventLog
	{
		std::vector<FCollisionEvent> Events;

		void Simulate(FPhysics2DSystem& Physics, FScene& Scene, float Seconds)
		{
			const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				Physics.Update(Scene, Frame);
				Scene.UpdateTransforms();
				Events.insert(Events.end(), Physics.GetCollisionEvents().begin(), Physics.GetCollisionEvents().end());
			}
		}
		int32 Count(ECollisionEventType Type, FEntity Self, FEntity Other = NullEntity) const
		{
			int32 Result = 0;
			for (const FCollisionEvent& Event : Events)
			{
				if (Event.Type == Type && Event.Self == Self && (!Other.IsValid() || Event.Other == Other))
				{
					++Result;
				}
			}
			return Result;
		}
	};

	void Simulate(FPhysics2DSystem& Physics, FScene& Scene, float Seconds)
	{
		FEventLog Log;
		Log.Simulate(Physics, Scene, Seconds);
	}
} // namespace

// 2D 카메라 규약: +Y 쪽에서 -Y를 보는 카메라(yaw -90)는 화면 오른쪽 = +X, 위 = +Z
E_TEST(Physics2D_CameraLooksAlongMinusY)
{
	FCameraComponent Camera;
	Camera.bOrthographic = true;
	Camera.OrthoHeight   = 1000.0f;
	const FQuat Rotation = FQuat::FromEuler(0.0f, -90.0f, 0.0f);
	const FVector3 Forward = Rotation.GetForwardVector();
	E_EXPECT_NEAR(Forward.Y, -1.0f, 1.0e-4f);
	const FMatrix4x4 ViewProjection = FCameraProjection::MakeViewProjection(FVector3(0.0f, 1000.0f, 0.0f), Rotation, Camera, 1.0f);
	const FVector2   Viewport(1000.0f, 1000.0f);
	FVector2         Center, Right, Up;
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(0.0f, 0.0f, 0.0f), Viewport, Center));
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(100.0f, 0.0f, 0.0f), Viewport, Right));
	E_EXPECT_TRUE(FCameraProjection::WorldToViewport(ViewProjection, FVector3(0.0f, 0.0f, 100.0f), Viewport, Up));
	E_EXPECT_TRUE(Right.X > Center.X + 50.0f); // +X = 화면 오른쪽
	E_EXPECT_TRUE(Up.Y < Center.Y - 50.0f);    // +Z = 화면 위 (픽셀 +Y는 아래)
}

// 2D 각(반시계 +) ↔ 월드 회전: 90도면 로컬 +X가 +Z(화면 위)를 본다. 왕복과 기운 회전 감지
E_TEST(Physics2D_RotationSignRoundTrip)
{
	const FQuat    Quarter = Physics2DMath::RotationFromAngle(FMath::HalfPi);
	const FVector3 LocalX  = Quarter.RotateVector(FVector3(1.0f, 0.0f, 0.0f));
	E_EXPECT_NEAR(LocalX.X, 0.0f, 1.0e-5f);
	E_EXPECT_NEAR(LocalX.Z, 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(Quarter.RotateVector(FVector3(0.0f, 1.0f, 0.0f)).Y, 1.0f, 1.0e-5f); // 깊이 축은 그대로

	const float Angles[] = { 0.0f, 0.3f, -1.2f, 2.5f, -3.0f };
	for (const float Angle : Angles)
	{
		bool        bTilted = true;
		const float Back    = Physics2DMath::AngleFromRotation(Physics2DMath::RotationFromAngle(Angle), &bTilted);
		E_EXPECT_NEAR(Back, Angle, 1.0e-4f);
		E_EXPECT_FALSE(bTilted);
	}
	bool bTilted = false;
	Physics2DMath::AngleFromRotation(FQuat::FromAxisAngle(FVector3(0.0f, 0.0f, 1.0f), 0.5f), &bTilted);
	E_EXPECT_TRUE(bTilted);
	const FVector2 Rotated = Physics2DMath::Rotate(FVector2(1.0f, 0.0f), FMath::HalfPi);
	E_EXPECT_NEAR(Rotated.Y, 1.0f, 1.0e-5f);
}

// 시스템 경계에서도 같은 부호: 각속도 +(반시계)로 돌린 바디의 엔티티 로컬 +X가 +Z 쪽으로 돈다
E_TEST(Physics2D_BodyAngleMatchesEntityRotation)
{
	FScene        Scene;
	const FEntity Box = AddBox(Scene, "Box", FVector3(0.0f, 30.0f, 0.0f));
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Box).GravityScale   = 0.0f;
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Box).AngularDamping = 0.0f;
	Scene.GetTransform(Box).Rotation = Physics2DMath::RotationFromAngle(0.4f);
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.SetInterpolation(false);
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	Physics.SetAngularVelocity(Box, 1.0f); // rad/s 반시계
	Simulate(Physics, Scene, 0.5f);
	const float Angle = Physics2DMath::AngleFromRotation(Scene.GetTransform(Box).Rotation);
	E_EXPECT_NEAR(Angle, 0.9f, 0.02f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Y, 30.0f, 1.0e-4f); // 깊이 유지
	Physics.End();
}

// 낙하 바디가 정적 바닥 위에 멈춘다 (상자 가운데 = 바닥 위 + 반 높이)
E_TEST(Physics2D_FallingBodyRestsOnGround)
{
	FScene Scene;
	AddGround(Scene);
	const FEntity Box  = AddBox(Scene, "Box", FVector3(0.0f, -20.0f, 300.0f));
	const FEntity Ball = AddBall(Scene, "Ball", FVector3(300.0f, 0.0f, 500.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 3.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 50.0f, 1.5f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.X, 0.0f, 0.5f);
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Y, -20.0f, 1.0e-4f);
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Z, 25.0f, 1.5f);
	E_EXPECT_TRUE(std::abs(Physics.GetVelocity(Box).Y) < 1.0f);
	E_EXPECT_NEAR(Physics.GetMass(Box), 100.0f, 0.5f); // 1m² × 기본 밀도 100 kg/m²
	Physics.End();
}

// 엔티티 스케일 X/Z가 모양에 곱해진다 (Y 무시): Z 스케일 2 → 높이 200cm 상자는 가운데 100cm에 선다
E_TEST(Physics2D_ScaleApplied)
{
	FScene Scene;
	AddGround(Scene);
	const FEntity Tall = AddBox(Scene, "Tall", FVector3(0.0f, 0.0f, 400.0f));
	Scene.GetTransform(Tall).Scale = FVector3(0.5f, 7.0f, 2.0f);
	const FEntity Wide = AddBall(Scene, "Wide", FVector3(500.0f, 0.0f, 400.0f), 20.0f);
	Scene.GetTransform(Wide).Scale = FVector3(3.0f, 1.0f, 1.0f); // 원은 X/Z 중 큰 값
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 3.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Tall).Position.Z, 100.0f, 2.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Wide).Position.Z, 60.0f, 2.0f);
	E_EXPECT_NEAR(Physics.GetMass(Tall), 100.0f, 0.5f); // 50 × 200cm
	Physics.End();
}

// 원웨이 플랫폼: 아래에서 위로 쏜 공은 통과하고, 떨어질 때는 위에 착지한다
E_TEST(Physics2D_OneWayPlatform)
{
	FScene Scene;
	AddGround(Scene);
	const FEntity Platform = Scene.CreateEntity("Platform");
	Scene.GetTransform(Platform).Position = FVector3(0.0f, 0.0f, 300.0f);
	FBoxCollider2DComponent& PlatformBox   = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Platform);
	PlatformBox.Size                       = FVector2(400.0f, 20.0f);
	PlatformBox.bOneWay                    = true;
	const FEntity Ball                     = AddBall(Scene, "Ball", FVector3(0.0f, 0.0f, 100.0f));
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Ball).bFixedRotation = true;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	Physics.SetVelocity(Ball, FVector2(0.0f, 1000.0f)); // 최고점 ≈ 100 + 1000²/(2·980) ≈ 610cm
	float MaxHeight = 0.0f;
	for (int32 Index = 0; Index < 180; ++Index)
	{
		Physics.Update(Scene, Frame);
		Scene.UpdateTransforms();
		MaxHeight = std::max(MaxHeight, Scene.GetTransform(Ball).Position.Z);
	}
	E_EXPECT_TRUE(MaxHeight > 500.0f);                                       // 막히지 않고 통과
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Z, 310.0f + 25.0f, 2.0f); // 판 위(300 + 10)에 착지
	Physics.End();
}

// 트리거: 막지 않고 들어옴/나감이 한 번씩 (양쪽). 정적 바닥과는 알리지 않고, 레이캐스트는 트리거를 무시한다
E_TEST(Physics2D_TriggerEnterExit)
{
	FScene        Scene;
	const FEntity Zone = Scene.CreateEntity("Zone");
	Scene.GetTransform(Zone).Position = FVector3(0.0f, 0.0f, 0.0f);
	FBoxCollider2DComponent& ZoneBox  = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Zone);
	ZoneBox.Size                      = FVector2(200.0f, 100.0f);
	ZoneBox.bIsTrigger                = true;
	const FEntity Ground              = AddGround(Scene, "Ground", -400.0f);
	const FEntity Ball                = AddBall(Scene, "Ball", FVector3(0.0f, 0.0f, 200.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	Physics.SetVelocity(Ball, FVector2(0.0f, -800.0f));
	FEventLog Log;
	Log.Simulate(Physics, Scene, 1.5f);
	E_EXPECT_NEAR(Scene.GetTransform(Ball).Position.Z, -375.0f, 2.0f); // 트리거를 지나 바닥에
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Zone, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Ball, Zone), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Zone, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Ball, Zone), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Zone, Ground), 0);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Ball), 0); // 보고 대상 아님

	FPhysics2DHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector2(0.0f, 200.0f), FVector2(0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Ball); // 트리거 건너뜀
	Physics.End();
}

// 충돌 알림: 보고 대상이면 시작이 한 번(양쪽, 법선 = 나를 상대에서 밀어내는 방향), 엔티티가 사라지면 끝
E_TEST(Physics2D_CollisionEvents)
{
	FScene        Scene;
	const FEntity Ground = AddGround(Scene);
	const FEntity Box    = AddBox(Scene, "Box", FVector3(0.0f, 40.0f, 200.0f));
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Box).bReportContacts = true;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	FEventLog Log;
	Log.Simulate(Physics, Scene, 1.5f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Box, Ground), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Ground, Box), 1);
	for (const FCollisionEvent& Event : Log.Events)
	{
		if (Event.Type == ECollisionEventType::CollisionBegin && Event.Self == Box)
		{
			E_EXPECT_NEAR(Event.Normal.Z, 1.0f, 0.01f); // 상자를 바닥에서 위로 밀어낸다
			E_EXPECT_NEAR(Event.Point.Z, 0.0f, 2.0f);
			E_EXPECT_NEAR(Event.Point.Y, 40.0f, 1.0e-4f); // 받는 엔티티 깊이
			E_EXPECT_TRUE(Event.ApproachSpeed > 100.0f);
		}
	}
	Scene.DestroyEntity(Box);
	Log.Events.clear();
	Log.Simulate(Physics, Scene, 0.1f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionEnd, Ground, Box), 1);
	Physics.End();
}

// 충돌 레이어 행렬: 꺼진 레이어끼리는 통과하고, 질의는 마스크로 거른다
E_TEST(Physics2D_CollisionLayersIgnore)
{
	FCollisionLayerSettings Layers;
	Layers.SetLayerName(1, "Ground");
	Layers.SetLayerName(2, "Ghost");
	Layers.SetCollision(1, 2, false);
	const FScopedCollisionLayers Scoped(Layers);

	FScene        Scene;
	const FEntity Ground = AddGround(Scene);
	Scene.GetRegistry().Get<FBoxCollider2DComponent>(Ground).Layer = "Ground";
	const FEntity Ghost = AddBox(Scene, "Ghost", FVector3(0.0f, 0.0f, 200.0f));
	Scene.GetRegistry().Get<FBoxCollider2DComponent>(Ghost).Layer = "Ghost";
	const FEntity Solid = AddBox(Scene, "Solid", FVector3(300.0f, 0.0f, 200.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 1.0f);
	E_EXPECT_TRUE(Scene.GetTransform(Ghost).Position.Z < -200.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Solid).Position.Z, 50.0f, 1.5f);

	uint32 GroundOnly = 0;
	E_EXPECT_TRUE(FProjectSettings::Get().Collision.MakeMask({ "Ground" }, GroundOnly));
	FPhysics2DHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector2(300.0f, 500.0f), FVector2(0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Solid);
	E_EXPECT_TRUE(Physics.Raycast(FVector2(300.0f, 500.0f), FVector2(0.0f, -1.0f), 1000.0f, Hit, GroundOnly));
	E_EXPECT_TRUE(Hit.Entity == Ground);
	Physics.End();
}

// 레이캐스트(점·법선·거리·비율)와 겹침(상자/원, 회전 상자)
E_TEST(Physics2D_RaycastAndOverlap)
{
	FScene        Scene;
	const FEntity Ground = AddGround(Scene);
	const FEntity Crate  = Scene.CreateEntity("Crate");
	Scene.GetTransform(Crate).Position = FVector3(500.0f, 0.0f, 100.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Crate).Size = FVector2(100.0f, 100.0f); // 정적
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);

	FPhysics2DHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector2(0.0f, 100.0f), FVector2(2.0f, 0.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Crate);
	E_EXPECT_NEAR(Hit.Position.X, 450.0f, 0.1f);
	E_EXPECT_NEAR(Hit.Normal.X, -1.0f, 1.0e-3f);
	E_EXPECT_NEAR(Hit.Distance, 450.0f, 0.1f);
	E_EXPECT_NEAR(Hit.Fraction, 0.45f, 1.0e-3f);
	E_EXPECT_FALSE(Physics.Raycast(FVector2(0.0f, 100.0f), FVector2(1.0f, 0.0f), 400.0f, Hit));

	std::vector<FEntity> Found;
	E_EXPECT_EQ(Physics.OverlapCircle(FVector2(500.0f, 100.0f), 10.0f, Found), 1u);
	E_EXPECT_TRUE(Found[0] == Crate);
	Found.clear();
	E_EXPECT_EQ(Physics.OverlapBox(FVector2(500.0f, 20.0f), FVector2(20.0f, 40.0f), 0.0f, Found), 2u); // 바닥과 상자 둘 다
	Found.clear();
	E_EXPECT_EQ(Physics.OverlapCircle(FVector2(200.0f, 200.0f), 30.0f, Found), 0u);
	// -45도 돌린 긴 상자: 끝이 상자 왼쪽 위 모서리(450, 150)를 파고든다 (돌리지 않으면 z 184 이상이라 닿지 않음)
	E_EXPECT_EQ(Physics.OverlapBox(FVector2(411.0f, 189.0f), FVector2(60.0f, 5.0f), 0.0f, Found), 0u);
	E_EXPECT_EQ(Physics.OverlapBox(FVector2(411.0f, 189.0f), FVector2(60.0f, 5.0f), -FMath::Pi * 0.25f, Found), 1u);
	(void)Ground;
	Physics.End();
}

// 고정 회전: 비스듬히 부딪혀도 각이 그대로
E_TEST(Physics2D_FixedRotation)
{
	FScene Scene;
	AddGround(Scene);
	const FEntity Ramp = Scene.CreateEntity("Ramp");
	Scene.GetTransform(Ramp).Position = FVector3(0.0f, 0.0f, 100.0f);
	Scene.GetTransform(Ramp).Rotation = Physics2DMath::RotationFromAngle(0.5f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Ramp).Size = FVector2(400.0f, 20.0f);
	const FEntity Free  = AddBox(Scene, "Free", FVector3(-80.0f, 0.0f, 300.0f), FVector2(40.0f, 40.0f));
	const FEntity Fixed = AddBox(Scene, "Fixed", FVector3(80.0f, 0.0f, 300.0f), FVector2(40.0f, 40.0f));
	Scene.GetRegistry().Get<FRigidBody2DComponent>(Fixed).bFixedRotation = true;
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 1.0f);
	E_EXPECT_NEAR(Physics2DMath::AngleFromRotation(Scene.GetTransform(Fixed).Rotation), 0.0f, 1.0e-4f);
	E_EXPECT_TRUE(std::abs(Physics2DMath::AngleFromRotation(Scene.GetTransform(Free).Rotation)) > 0.2f); // 경사면에서 기운다
	Physics.End();
}

// 키네마틱: 트랜스폼을 따라가고, 위에 놓인 동적 상자를 밀어 올린다
E_TEST(Physics2D_KinematicFollowsTransform)
{
	FScene        Scene;
	const FEntity Lift = Scene.CreateEntity("Lift");
	Scene.GetTransform(Lift).Position = FVector3(0.0f, 0.0f, 0.0f);
	Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Lift).Size = FVector2(400.0f, 20.0f);
	Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Lift).BodyType = EBodyType2D::Kinematic;
	const FEntity Box = AddBox(Scene, "Box", FVector3(0.0f, 0.0f, 60.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 0.5f); // 내려앉기
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Scene.GetTransform(Lift).Position.Z += 2.0f; // 120cm/s로 올린다
		Physics.Update(Scene, Frame);
		Scene.UpdateTransforms();
	}
	E_EXPECT_NEAR(Scene.GetTransform(Lift).Position.Z, 120.0f, 1.0e-3f); // 키네마틱은 물리가 트랜스폼을 쓰지 않는다
	E_EXPECT_NEAR(Scene.GetTransform(Box).Position.Z, 120.0f + 10.0f + 50.0f, 3.0f);
	Physics.End();
}

// 다각형 문자열 파서·볼록 껍질·8점 줄이기, 오목 다각형 콜라이더는 껍질로 동작
E_TEST(Physics2D_PolygonPointParser)
{
	std::vector<FVector2> Points;
	E_EXPECT_TRUE(Physics2DMath::ParsePoints(" -50, -50 ; 50,-50;0 ,50; ", Points));
	E_EXPECT_EQ(Points.size(), size_t(3));
	E_EXPECT_NEAR(Points[0].X, -50.0f, 0.0f);
	E_EXPECT_NEAR(Points[2].Y, 50.0f, 0.0f);
	E_EXPECT_TRUE(Physics2DMath::ParsePoints("", Points));
	E_EXPECT_TRUE(Points.empty());
	std::string Error;
	E_EXPECT_FALSE(Physics2DMath::ParsePoints("1,2; 3", Points, &Error));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_FALSE(Physics2DMath::ParsePoints("1,2; a,4", Points));
	E_EXPECT_TRUE(Physics2DMath::ParsePoints("1.5,-2e1", Points));
	E_EXPECT_NEAR(Points[0].Y, -20.0f, 0.0f);
	std::vector<FVector2> Round;
	E_EXPECT_TRUE(Physics2DMath::ParsePoints(Physics2DMath::FormatPoints({ FVector2(1.25f, -3.0f), FVector2(4.0f, 5.5f) }), Round));
	E_EXPECT_NEAR(Round[1].Y, 5.5f, 0.0f);

	// 오목(화살표) → 껍질 4점, 반시계
	E_EXPECT_TRUE(Physics2DMath::ParsePoints("0,0; 100,0; 50,20; 100,100; 0,100", Points));
	E_EXPECT_FALSE(Physics2DMath::IsConvexPolygon(Points));
	const std::vector<FVector2> Hull = Physics2DMath::ComputeConvexHull(Points);
	E_EXPECT_EQ(Hull.size(), size_t(4));
	E_EXPECT_TRUE(Physics2DMath::SignedArea(Hull) > 0.0f);
	E_EXPECT_TRUE(Physics2DMath::IsConvexPolygon(Hull));
	// 원 위 16점 → 8점
	std::vector<FVector2> Circle;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const float Angle = FMath::TwoPi * static_cast<float>(Index) / 16.0f;
		Circle.push_back(FVector2(std::cos(Angle) * 100.0f, std::sin(Angle) * 100.0f));
	}
	const std::vector<FVector2> Reduced = Physics2DMath::ReduceConvexPolygon(Physics2DMath::ComputeConvexHull(Circle), 8);
	E_EXPECT_EQ(Reduced.size(), size_t(8));
	E_EXPECT_TRUE(Physics2DMath::IsConvexPolygon(Reduced));

	// 다각형 콜라이더 (오목 → 껍질 경고 후 동작)
	FScene        Scene;
	const FEntity Wedge = Scene.CreateEntity("Wedge");
	Scene.GetRegistry().Emplace<FPolygonCollider2DComponent>(Wedge).Points = "-100,0; 100,0; 0,10; 0,100";
	const FEntity Ball = AddBall(Scene, "Ball", FVector3(0.0f, 0.0f, 300.0f), 10.0f);
	Scene.UpdateTransforms();
	FPhysics2DSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	E_EXPECT_EQ(Physics.GetBodyCount(), 2u);
	FPhysics2DHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(FVector2(0.0f, 400.0f), FVector2(0.0f, -1.0f), 400.0f, Hit) && Hit.Entity == Ball);
	E_EXPECT_TRUE(Physics.Raycast(FVector2(-50.0f, 300.0f), FVector2(0.0f, -1.0f), 400.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Wedge);
	E_EXPECT_NEAR(Hit.Position.Y, 50.0f, 0.5f); // 껍질 왼쪽 빗변 (-100,0)-(0,100)
	Physics.End();
}

// 선분 체인: 열린 선분 위에 착지, 닫힌 고리(시계 방향으로 줘도 바깥쪽이 막힘)
E_TEST(Physics2D_EdgeChain)
{
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetRegistry().Emplace<FEdgeCollider2DComponent>(Floor).Points = "-500,0; 0,-50; 500,0";
	const FEntity Rock = Scene.CreateEntity("Rock");
	Scene.GetTransform(Rock).Position = FVector3(1000.0f, 0.0f, 0.0f);
	FEdgeCollider2DComponent& Loop    = Scene.GetRegistry().Emplace<FEdgeCollider2DComponent>(Rock);
	Loop.Points                       = "-100,-100; -100,100; 100,100; 100,-100"; // 시계 방향
	Loop.bLoop                        = true;
	const FEntity BallA               = AddBall(Scene, "BallA", FVector3(0.0f, 0.0f, 200.0f));
	const FEntity BallB               = AddBall(Scene, "BallB", FVector3(1000.0f, 0.0f, 300.0f));
	Scene.UpdateTransforms();

	FPhysics2DSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 3.0f);
	E_EXPECT_NEAR(Scene.GetTransform(BallA).Position.Z, -50.0f + 25.0f, 3.0f); // 골짜기 바닥
	E_EXPECT_NEAR(Scene.GetTransform(BallA).Position.X, 0.0f, 5.0f);
	E_EXPECT_NEAR(Scene.GetTransform(BallB).Position.Z, 100.0f + 25.0f, 2.0f); // 고리 윗면
	Physics.End();
}

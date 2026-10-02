#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <vector>

// 겹침 검사 / 쓸어 보기 (Phase 41-2, FPhysicsWorld::Overlap/Sweep, FPhysicsSystem::Overlap*/ *Cast)
namespace
{
	constexpr float QueryTol = 0.5f; // cm (Jolt 볼록 반지름/수치 오차)

	uint32 AddBox(FPhysicsWorld& World, const FVector3& Position, const FVector3& HalfExtents, EPhysicsMotionType Motion, uint64 UserData,
	              bool bTrigger = false)
	{
		FPhysicsBodyDesc Desc;
		Desc.MotionType  = Motion;
		Desc.Position    = Position;
		Desc.Shape       = EPhysicsShape::Box;
		Desc.HalfExtents = HalfExtents;
		Desc.bUseGravity = false;
		Desc.UserData    = UserData;
		Desc.bIsTrigger  = bTrigger;
		return World.CreateBody(Desc);
	}

	uint32 AddSphere(FPhysicsWorld& World, const FVector3& Position, float Radius, uint64 UserData)
	{
		FPhysicsBodyDesc Desc;
		Desc.MotionType  = EPhysicsMotionType::Dynamic;
		Desc.Position    = Position;
		Desc.Shape       = EPhysicsShape::Sphere;
		Desc.Radius      = Radius;
		Desc.bUseGravity = false;
		Desc.UserData    = UserData;
		return World.CreateBody(Desc);
	}

	bool Contains(const std::vector<uint64>& Values, uint64 Value)
	{
		return std::find(Values.begin(), Values.end(), Value) != Values.end();
	}
} // namespace

E_TEST(PhysicsQuery_OverlapSphereBoxAndTriggerExcluded)
{
	FPhysicsWorld World;
	AddBox(World, FVector3::ZeroVector, FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, 1);
	AddSphere(World, FVector3(0.0f, 300.0f, 0.0f), 25.0f, 2);
	AddBox(World, FVector3(0.0f, -300.0f, 0.0f), FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, 3, true); // 트리거

	std::vector<uint64> Hits;
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(60.0f), FVector3(100.0f, 0.0f, 0.0f), FQuat::Identity, Hits), 1u); // x 40까지 → 면 50과 겹침
	E_EXPECT_TRUE(Hits.size() == 1 && Hits[0] == 1);
	Hits.clear();
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(40.0f), FVector3(100.0f, 0.0f, 0.0f), FQuat::Identity, Hits), 0u); // x 60까지
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(10.0f), FVector3(0.0f, 300.0f, 0.0f), FQuat::Identity, Hits), 1u);
	E_EXPECT_TRUE(Contains(Hits, 2));
	Hits.clear();
	// 트리거는 질의에서 빠진다 (레이캐스트와 같음)
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(100.0f), FVector3(0.0f, -300.0f, 0.0f), FQuat::Identity, Hits), 0u);
	// 큰 상자: 둘 다 (바디마다 한 번)
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeBox(FVector3(100.0f, 400.0f, 100.0f)), FVector3::ZeroVector, FQuat::Identity, Hits), 2u);
	E_EXPECT_TRUE(Contains(Hits, 1) && Contains(Hits, 2) && !Contains(Hits, 3));
	// 회전한 상자: 가운데 x 110, 반 크기 50 → 회전 없으면 왼쪽 면 x 60 (안 닿음), 45도 돌리면 모서리 x 110 - 70.7 = 39 (닿음)
	Hits.clear();
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeBox(FVector3(50.0f, 50.0f, 50.0f)), FVector3(110.0f, 0.0f, 0.0f), FQuat::Identity, Hits), 0u);
	const FQuat Yaw45 = FQuat::FromAxisAngle(FVector3::UpVector, FMath::Pi * 0.25f);
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeBox(FVector3(50.0f, 50.0f, 50.0f)), FVector3(110.0f, 0.0f, 0.0f), Yaw45, Hits), 1u);
}

E_TEST(PhysicsQuery_CapsuleIsZUp)
{
	FPhysicsWorld World;
	AddBox(World, FVector3::ZeroVector, FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, 1); // 윗면 z = 50
	std::vector<uint64>      Hits;
	const FPhysicsQueryShape Capsule = FPhysicsQueryShape::MakeCapsule(10.0f, 100.0f); // 전체 높이 220
	// 세운 캡슐 (가운데 z 150 → 아래 끝 z 40): 닿음. 눕히면 (Y축 90도) 아래 z 140: 안 닿음
	E_EXPECT_EQ(World.Overlap(Capsule, FVector3(0.0f, 0.0f, 150.0f), FQuat::Identity, Hits), 1u);
	const FQuat Lying = FQuat::FromAxisAngle(FVector3::RightVector, FMath::Pi * 0.5f);
	E_EXPECT_EQ(World.Overlap(Capsule, FVector3(0.0f, 0.0f, 150.0f), Lying, Hits), 0u);
	// 세운 캡슐을 위에서 쓸어 내리면 아래 끝(가운데 - 110)이 z 50에 닿는다
	FPhysicsRayHit Hit;
	E_EXPECT_TRUE(World.Sweep(Capsule, FVector3(0.0f, 0.0f, 500.0f), FQuat::Identity, FVector3(0.0f, 0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_NEAR(Hit.Distance, 500.0f - 110.0f - 50.0f, QueryTol);
	E_EXPECT_TRUE(Hit.UserData == 1);
}

E_TEST(PhysicsQuery_SweepDistanceNormalAndLimits)
{
	FPhysicsWorld World;
	const uint32 Floor = AddBox(World, FVector3::ZeroVector, FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, 1);
	(void)Floor;
	AddBox(World, FVector3(500.0f, 0.0f, 300.0f), FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, 9, true); // 트리거 (경로 위)

	FPhysicsRayHit Hit;
	// 구 반지름 10을 z 500에서 아래로: 윗면 50 + 반지름 10 = 가운데 60에서 닿음 → 440 이동
	E_EXPECT_TRUE(World.Sweep(FPhysicsQueryShape::MakeSphere(10.0f), FVector3(20.0f, -10.0f, 500.0f), FQuat::Identity, FVector3(0.0f, 0.0f, -3.0f), 1000.0f, Hit));
	E_EXPECT_NEAR(Hit.Distance, 440.0f, QueryTol);
	E_EXPECT_EQUALS(Hit.Normal, FVector3(0.0f, 0.0f, 1.0f), 0.01f);
	E_EXPECT_EQUALS(Hit.Position, FVector3(20.0f, -10.0f, 50.0f), QueryTol);
	E_EXPECT_TRUE(Hit.UserData == 1);
	// 거리 제한 / 반대 방향
	E_EXPECT_FALSE(World.Sweep(FPhysicsQueryShape::MakeSphere(10.0f), FVector3(0.0f, 0.0f, 500.0f), FQuat::Identity, FVector3(0.0f, 0.0f, -1.0f), 400.0f, Hit));
	E_EXPECT_FALSE(World.Sweep(FPhysicsQueryShape::MakeSphere(10.0f), FVector3(0.0f, 0.0f, 500.0f), FQuat::Identity, FVector3(0.0f, 0.0f, 1.0f), 1000.0f, Hit));
	// 상자를 옆에서: 오른쪽 면 x 50, 상자 반 크기 20 → 가운데 x 70에서 닿음
	E_EXPECT_TRUE(World.Sweep(FPhysicsQueryShape::MakeBox(FVector3(20.0f, 20.0f, 20.0f)), FVector3(300.0f, 0.0f, 0.0f), FQuat::Identity,
	                          FVector3(-1.0f, 0.0f, 0.0f), 1000.0f, Hit));
	E_EXPECT_NEAR(Hit.Distance, 230.0f, QueryTol);
	E_EXPECT_EQUALS(Hit.Normal, FVector3(1.0f, 0.0f, 0.0f), 0.01f);
	// 시작부터 겹침 → 거리 0
	E_EXPECT_TRUE(World.Sweep(FPhysicsQueryShape::MakeSphere(10.0f), FVector3(0.0f, 0.0f, 40.0f), FQuat::Identity, FVector3(0.0f, 0.0f, -1.0f), 100.0f, Hit));
	E_EXPECT_NEAR(Hit.Distance, 0.0f, QueryTol);
	// 트리거는 통과한다 (경로 위 트리거 → 아무것도 없음)
	E_EXPECT_FALSE(World.Sweep(FPhysicsQueryShape::MakeSphere(10.0f), FVector3(500.0f, 0.0f, 1000.0f), FQuat::Identity, FVector3(0.0f, 0.0f, -1.0f), 900.0f, Hit));
}

E_TEST(PhysicsQuery_IgnoreBodyAndDisabledPairs)
{
	FPhysicsWorld World;
	const uint32 A = AddSphere(World, FVector3(0.0f, 0.0f, 0.0f), 20.0f, 10);
	const uint32 B = AddSphere(World, FVector3(30.0f, 0.0f, 0.0f), 20.0f, 11);
	AddSphere(World, FVector3(-30.0f, 0.0f, 0.0f), 20.0f, 12);
	World.DisableCollision(A, B); // 관절로 이은 쌍처럼

	std::vector<uint64> Hits;
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(100.0f), FVector3::ZeroVector, FQuat::Identity, Hits), 3u);
	Hits.clear();
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(100.0f), FVector3::ZeroVector, FQuat::Identity, Hits, A), 1u); // A 자신 + 꺼진 쌍 B 제외
	E_EXPECT_TRUE(Hits.size() == 1 && Hits[0] == 12);
	World.EnableCollision(A, B);
	Hits.clear();
	E_EXPECT_EQ(World.Overlap(FPhysicsQueryShape::MakeSphere(100.0f), FVector3::ZeroVector, FQuat::Identity, Hits, A), 2u);

	// 쓸어 보기: 앞을 가로막는 바디를 무시하면 그 뒤를 맞힌다
	FPhysicsRayHit Hit;
	E_EXPECT_TRUE(World.Sweep(FPhysicsQueryShape::MakeSphere(5.0f), FVector3(300.0f, 0.0f, 0.0f), FQuat::Identity, FVector3(-1.0f, 0.0f, 0.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.UserData == 11);
	E_EXPECT_TRUE(World.Sweep(FPhysicsQueryShape::MakeSphere(5.0f), FVector3(300.0f, 0.0f, 0.0f), FQuat::Identity, FVector3(-1.0f, 0.0f, 0.0f), 1000.0f, Hit, B));
	E_EXPECT_TRUE(Hit.UserData == 10);
}

E_TEST(PhysicsSystem_ShapeQueriesReturnEntities)
{
	FScene        Scene;
	const FEntity Floor = Scene.CreateEntity("Floor");
	Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Floor).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);

	const FEntity Crate = Scene.CreateEntity("Crate");
	Scene.GetTransform(Crate).Position = FVector3(200.0f, 0.0f, 50.0f);
	Scene.GetRegistry().Emplace<FBoxColliderComponent>(Crate).HalfExtents = FVector3(50.0f, 50.0f, 50.0f);
	Scene.GetRegistry().Emplace<FRigidBodyComponent>(Crate).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);

	const FEntity Zone = Scene.CreateEntity("Zone");
	Scene.GetTransform(Zone).Position = FVector3(-200.0f, 0.0f, 50.0f);
	FBoxColliderComponent& ZoneBox    = Scene.GetRegistry().Emplace<FBoxColliderComponent>(Zone);
	ZoneBox.HalfExtents               = FVector3(50.0f, 50.0f, 50.0f);
	ZoneBox.bIsTrigger                = true;

	const FEntity Hero = Scene.CreateEntity("Hero");
	Scene.GetTransform(Hero).Position = FVector3(0.0f, 0.0f, 90.0f); // 캡슐 중심 (서 있는 높이)
	Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Hero);

	Scene.UpdateTransforms();
	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 1.0f / 60.0f);
	Scene.UpdateTransforms();

	// 구 하나로 바닥 + 상자 + 캐릭터(내부 바디). 트리거는 없음
	std::vector<FEntity> Found;
	E_EXPECT_EQ(Physics.OverlapSphere(FVector3(0.0f, 0.0f, 50.0f), 300.0f, Found), 3u);
	const auto Has = [&Found](FEntity Entity) { return std::find(Found.begin(), Found.end(), Entity) != Found.end(); };
	E_EXPECT_TRUE(Has(Floor) && Has(Crate) && Has(Hero) && !Has(Zone));
	// 캐릭터 자신 제외 (내부 바디로 찾는다)
	Found.clear();
	E_EXPECT_EQ(Physics.OverlapSphere(FVector3(0.0f, 0.0f, 50.0f), 300.0f, Found, Hero), 2u);
	E_EXPECT_FALSE(Has(Hero));
	Found.clear();
	E_EXPECT_EQ(Physics.OverlapBox(FVector3(200.0f, 0.0f, 150.0f), FVector3(10.0f, 10.0f, 60.0f), FQuat::Identity, Found), 1u);
	E_EXPECT_TRUE(Found.size() == 1 && Found[0] == Crate);
	Found.clear();
	E_EXPECT_EQ(Physics.OverlapCapsule(FVector3(200.0f, 0.0f, 300.0f), 10.0f, 100.0f, FQuat::Identity, Found), 0u); // 아래 끝 z 190 > 100

	// 캐릭터 위치에서 상자 쪽으로 구를 쓸어 보면 캐릭터 자신은 무시하고 상자 왼쪽 면(x 150)
	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.SphereCast(FVector3(0.0f, 0.0f, 90.0f), 20.0f, FVector3(1.0f, 0.0f, 0.0f), 1000.0f, Hit, Hero));
	E_EXPECT_TRUE(Hit.Entity == Crate);
	E_EXPECT_NEAR(Hit.Distance, 130.0f, QueryTol);
	E_EXPECT_TRUE(Physics.BoxCast(FVector3(200.0f, 0.0f, 400.0f), FVector3(10.0f, 10.0f, 10.0f), FQuat::Identity, FVector3(0.0f, 0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Crate);
	E_EXPECT_NEAR(Hit.Distance, 290.0f, QueryTol);
	E_EXPECT_TRUE(Physics.CapsuleCast(FVector3(-200.0f, 0.0f, 400.0f), 10.0f, 20.0f, FQuat::Identity, FVector3(0.0f, 0.0f, -1.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Floor); // 트리거 통과 → 바닥 (아래 끝 = 가운데 - 30)
	E_EXPECT_NEAR(Hit.Distance, 370.0f, QueryTol);
	Physics.End();
}

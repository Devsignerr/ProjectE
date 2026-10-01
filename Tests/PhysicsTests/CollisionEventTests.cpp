// 충돌·트리거 알림 (Physics/PhysicsSystem.h "충돌 알림"): 쌍 단위 시작/끝 한 번, 잠들어도 끝 아님, 보고 대상 필터, 트리거 센서
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <vector>

namespace
{
	constexpr float Frame = 1.0f / 60.0f;

	FEntity AddFloor(FScene& Scene)
	{
		const FEntity Floor = Scene.CreateEntity("Floor");
		Scene.GetTransform(Floor).Position = FVector3(0.0f, 0.0f, -10.0f);
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Floor).HalfExtents = FVector3(1000.0f, 1000.0f, 10.0f);
		return Floor; // 강체 없음 = 정적
	}

	FEntity AddBall(FScene& Scene, const char* Name, const FVector3& Position, bool bReport)
	{
		const FEntity Ball = Scene.CreateEntity(Name);
		Scene.GetTransform(Ball).Position = Position;
		Scene.GetRegistry().Emplace<FSphereColliderComponent>(Ball).Radius = 25.0f;
		FRigidBodyComponent& Body = Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ball);
		Body.Mass                 = 1.0f;
		Body.Restitution          = 0.0f;
		Body.bReportContacts      = bReport;
		return Ball;
	}

	// 지난 Update들에서 쌓인 이벤트를 모아 둔다 (Update마다 비워지므로 프레임마다 복사)
	struct FEventLog
	{
		std::vector<FCollisionEvent> Events;

		void Simulate(FPhysicsSystem& Physics, FScene& Scene, float Seconds)
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
		const FCollisionEvent* Find(ECollisionEventType Type, FEntity Self) const
		{
			for (const FCollisionEvent& Event : Events)
			{
				if (Event.Type == Type && Event.Self == Self)
				{
					return &Event;
				}
			}
			return nullptr;
		}
	};
} // namespace

// 떨어진 공: 바닥과 시작 한 번(양쪽), 법선/지점/충격 세기 추정. 굴러 멈춰 잠들어도 끝이 아니고, 들어 올리면 끝 한 번
E_TEST(PhysicsEvents_CollisionBeginEndOncePerPair)
{
	FScene        Scene;
	const FEntity Floor = AddFloor(Scene);
	const FEntity Ball  = AddBall(Scene, "Ball", FVector3(0.0f, 0.0f, 100.0f), true);
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	Physics.SetVelocity(Ball, FVector3(0.0f, 0.0f, -500.0f));
	FEventLog Log;
	Log.Simulate(Physics, Scene, 4.0f); // 닿고 → 잠든다 (Jolt 기본 0.5초)
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Ball, Floor), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Floor, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionEnd, Ball), 0); // 잠들어 생긴 Jolt 제거 통지는 끝이 아니다

	const FCollisionEvent* Hit = Log.Find(ECollisionEventType::CollisionBegin, Ball);
	E_EXPECT_TRUE(Hit != nullptr);
	if (Hit != nullptr)
	{
		E_EXPECT_TRUE(Hit->Normal.Z > 0.99f);              // 공을 바닥에서 밀어내는 방향 = 위
		E_EXPECT_NEAR(Hit->Point.Z, 0.0f, 3.0f);           // 바닥 윗면
		E_EXPECT_TRUE(Hit->ApproachSpeed > 450.0f && Hit->ApproachSpeed < 700.0f); // 500cm/s + 낙하 가속
		E_EXPECT_NEAR(Hit->Impulse, Hit->ApproachSpeed * 1.0f, 1.0f); // 1kg 대 정적 = 속력 × 1kg
		const FCollisionEvent* FloorSide = Log.Find(ECollisionEventType::CollisionBegin, Floor);
		E_EXPECT_TRUE(FloorSide != nullptr && FloorSide->Normal.Z < -0.99f);
	}

	// 순간이동으로 떼어 놓으면 끝 한 번 (양쪽)
	Scene.GetTransform(Ball).Position = FVector3(0.0f, 0.0f, 500.0f);
	Scene.UpdateTransforms();
	Log.Events.clear();
	Log.Simulate(Physics, Scene, 0.1f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionEnd, Ball, Floor), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionEnd, Floor, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Ball), 0);
	Physics.End();
}

// 보고 대상이 아니면 이벤트가 없다. 필터로 켤 수 있고, 엔티티가 사라지면 그 쌍의 끝이 바로 생긴다
E_TEST(PhysicsEvents_ReportFilterAndDestroyedBody)
{
	FScene        Scene;
	const FEntity Floor  = AddFloor(Scene);
	const FEntity Quiet  = AddBall(Scene, "Quiet", FVector3(0.0f, 0.0f, 30.0f), false);
	const FEntity Filter = AddBall(Scene, "Filtered", FVector3(200.0f, 0.0f, 30.0f), false);
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.SetContactReportFilter([Filter](const FScene&, FEntity Entity) { return Entity == Filter; });
	Physics.Begin();
	FEventLog Log;
	Log.Simulate(Physics, Scene, 0.5f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Quiet), 0);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Floor, Quiet), 0);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Filter, Floor), 1);

	Scene.DestroyEntity(Filter);
	Log.Events.clear();
	Log.Simulate(Physics, Scene, 0.1f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionEnd, Floor, Filter), 1); // 남은 쪽이 받는다 (상대는 이미 없음)
	E_EXPECT_EQ(static_cast<int32>(Log.Events.size()), 2);
	Physics.End();
}

// 트리거: 공은 막히지 않고 통과하며 들어옴/나감이 한 번씩 (양쪽). 레이캐스트는 트리거를 무시한다
E_TEST(PhysicsEvents_TriggerEnterExitWithoutBlocking)
{
	FScene        Scene;
	const FEntity Zone = Scene.CreateEntity("Zone");
	Scene.GetTransform(Zone).Position = FVector3(0.0f, 0.0f, 0.0f);
	FBoxColliderComponent& ZoneBox    = Scene.GetRegistry().Emplace<FBoxColliderComponent>(Zone);
	ZoneBox.HalfExtents               = FVector3(100.0f, 100.0f, 50.0f);
	ZoneBox.bIsTrigger                = true;
	const FEntity Ball                = AddBall(Scene, "Ball", FVector3(0.0f, 0.0f, 200.0f), false); // 트리거가 보고 대상이라 공은 꺼 둬도 된다
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	Physics.SetVelocity(Ball, FVector3(0.0f, 0.0f, -800.0f));
	FEventLog Log;
	Log.Simulate(Physics, Scene, 1.0f);
	E_EXPECT_TRUE(Scene.GetTransform(Ball).Position.Z < -200.0f); // 통과
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Zone, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Ball, Zone), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Zone, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Ball, Zone), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::CollisionBegin, Ball), 0);

	FPhysicsHit Hit;
	E_EXPECT_FALSE(Physics.Raycast(FVector3(0.0f, 0.0f, 500.0f), FVector3(0.0f, 0.0f, -1.0f), 520.0f, Hit)); // 트리거만 있음
	Physics.End();
}

// 트리거 안에서 잠든 물체는 나가지 않은 것으로 남고, 캐릭터(내부 키네마틱 바디)도 감지한다
E_TEST(PhysicsEvents_TriggerKeepsSleepingBodyAndDetectsCharacter)
{
	FScene        Scene;
	AddFloor(Scene);
	const FEntity Zone = Scene.CreateEntity("Zone");
	Scene.GetTransform(Zone).Position = FVector3(0.0f, 0.0f, 100.0f);
	FBoxColliderComponent& ZoneBox    = Scene.GetRegistry().Emplace<FBoxColliderComponent>(Zone);
	ZoneBox.HalfExtents               = FVector3(150.0f, 150.0f, 100.0f);
	ZoneBox.bIsTrigger                = true;
	const FEntity Ball                = AddBall(Scene, "Ball", FVector3(0.0f, 0.0f, 30.0f), false);
	const FEntity Hero                = Scene.CreateEntity("Hero");
	Scene.GetTransform(Hero).Position = FVector3(800.0f, 0.0f, 100.0f);
	Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Hero);
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	FEventLog Log;
	Log.Simulate(Physics, Scene, 3.0f); // 공은 바닥에서 잠든다
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Zone, Ball), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Zone, Ball), 0);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Zone, Hero), 0);

	// 캐릭터를 영역 안으로 → 들어옴, 다시 밖으로 → 나감
	Physics.SetCharacterState(Scene, Hero, { FVector3(0.0f, 100.0f, 100.0f), FVector3(), false });
	Log.Events.clear();
	Log.Simulate(Physics, Scene, 0.1f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Zone, Hero), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerEnter, Hero, Zone), 1);
	Physics.SetCharacterState(Scene, Hero, { FVector3(800.0f, 0.0f, 100.0f), FVector3(), false });
	Log.Events.clear();
	Log.Simulate(Physics, Scene, 0.1f);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Zone, Hero), 1);
	E_EXPECT_EQ(Log.Count(ECollisionEventType::TriggerExit, Zone, Ball), 0);
	Physics.End();
}

// 충돌 레이어 (Phase 43, 유니티식 — Core/Settings/CollisionSettings.h): 이름 → 칸, 대칭 행렬, JSON, 바디/캐릭터/트리거/레이캐스트 거르기
#include "Core/Settings/CollisionSettings.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	constexpr float Frame      = 1.0f / 60.0f;
	constexpr float StandingZ  = 90.0f; // 캐릭터 기본 캡슐 (반지름 35 + 원기둥 절반 55)

	// 테스트 동안 프로젝트 설정 충돌 레이어를 바꾸고 끝나면 되돌린다 (FPhysicsSystem::Begin이 읽는다)
	struct FScopedCollisionLayers
	{
		FCollisionLayerSettings Saved;
		explicit FScopedCollisionLayers(const FCollisionLayerSettings& Layers) : Saved(FProjectSettings::Get().Collision)
		{
			FProjectSettings::Get().Collision = Layers;
		}
		~FScopedCollisionLayers() { FProjectSettings::Get().Collision = Saved; }
	};

	// Default / Ground / Prop / Ghost(Ground·Prop과 안 부딪힘) / Character(캐릭터끼리 통과) / Sensor(Prop과 안 부딪힘)
	FCollisionLayerSettings MakeTestLayers()
	{
		FCollisionLayerSettings Layers;
		Layers.SetLayerName(1, "Ground");
		Layers.SetLayerName(2, "Prop");
		Layers.SetLayerName(3, "Ghost");
		Layers.SetLayerName(4, "Character");
		Layers.SetLayerName(5, "Sensor");
		Layers.SetCollision(3, 1, false);
		Layers.SetCollision(3, 2, false);
		Layers.SetCollision(4, 4, false);
		Layers.SetCollision(4, 2, false);
		Layers.SetCollision(5, 2, false);
		return Layers;
	}

	FEntity AddBox(FScene& Scene, const char* Name, const FVector3& Center, const FVector3& HalfExtents, EPhysicsMotionType Motion, const char* Layer)
	{
		const FEntity Entity = Scene.CreateEntity(Name);
		Scene.GetTransform(Entity).Position = Center;
		FBoxColliderComponent& Collider = Scene.GetRegistry().Emplace<FBoxColliderComponent>(Entity);
		Collider.HalfExtents            = HalfExtents;
		Collider.Layer                  = Layer;
		FRigidBodyComponent& Body       = Scene.GetRegistry().Emplace<FRigidBodyComponent>(Entity);
		Body.MotionType                 = static_cast<int32>(Motion);
		Body.Mass                       = 1.0f;
		return Entity;
	}

	FEntity AddBall(FScene& Scene, const char* Name, const FVector3& Position, const char* Layer)
	{
		const FEntity Ball = Scene.CreateEntity(Name);
		Scene.GetTransform(Ball).Position = Position;
		FSphereColliderComponent& Collider = Scene.GetRegistry().Emplace<FSphereColliderComponent>(Ball);
		Collider.Radius                    = 25.0f;
		Collider.Layer                     = Layer;
		Scene.GetRegistry().Emplace<FRigidBodyComponent>(Ball).Mass = 1.0f;
		return Ball;
	}

	FEntity AddCharacter(FScene& Scene, const char* Name, const FVector3& Position, const char* Layer)
	{
		const FEntity Character = Scene.CreateEntity(Name);
		Scene.GetTransform(Character).Position = Position;
		Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Character).Layer = Layer;
		return Character;
	}

	void Simulate(FPhysicsSystem& Physics, FScene& Scene, float Seconds, std::vector<FCollisionEvent>* OutEvents = nullptr)
	{
		const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
			if (OutEvents != nullptr)
			{
				OutEvents->insert(OutEvents->end(), Physics.GetCollisionEvents().begin(), Physics.GetCollisionEvents().end());
			}
		}
	}

	void WalkCharacters(FPhysicsSystem& Physics, FScene& Scene, std::initializer_list<std::pair<FEntity, FVector2>> Inputs, float Seconds)
	{
		const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			for (const auto& [Character, Input] : Inputs)
			{
				FCharacterMove Move;
				Move.DeltaSeconds = Frame;
				Move.Input        = Input;
				Physics.SimulateCharacter(Scene, Character, Move);
			}
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
	}

	int32 CountEvents(const std::vector<FCollisionEvent>& Events, ECollisionEventType Type, FEntity Self)
	{
		int32 Count = 0;
		for (const FCollisionEvent& Event : Events)
		{
			Count += Event.Type == Type && Event.Self == Self ? 1 : 0;
		}
		return Count;
	}
} // namespace

// 기본 = Default 하나, 모두 충돌. 비었거나 없는 이름 = Default. 행렬은 대칭, 이름을 바꾸면 그 칸은 모두 켬으로
E_TEST(CollisionLayers_NamesMatrixAndMask)
{
	FCollisionLayerSettings Layers;
	E_EXPECT_TRUE(Layers.GetLayerName(0) == "Default");
	E_EXPECT_EQ(Layers.GetLayerNames().size(), size_t(1));
	for (uint32 A = 0; A < FCollisionLayerSettings::MaxLayers; ++A)
	{
		for (uint32 B = 0; B < FCollisionLayerSettings::MaxLayers; ++B)
		{
			E_EXPECT_TRUE(Layers.ShouldCollide(A, B));
		}
	}
	E_EXPECT_FALSE(Layers.SetLayerName(0, "Renamed")); // Default 고정
	E_EXPECT_EQ(Layers.ResolveLayer(""), 0u);
	E_EXPECT_EQ(Layers.ResolveLayer("Missing"), 0u);
	E_EXPECT_EQ(Layers.FindLayer("Missing"), -1);

	Layers = MakeTestLayers();
	E_EXPECT_EQ(Layers.ResolveLayer("Prop"), 2u);
	E_EXPECT_FALSE(Layers.ShouldCollide(1, 3));
	E_EXPECT_FALSE(Layers.ShouldCollide(3, 1)); // 대칭
	E_EXPECT_FALSE(Layers.ShouldCollide(4, 4));
	E_EXPECT_TRUE(Layers.ShouldCollide(0, 3));
	Layers.SetLayerName(3, "Spirit"); // 다른 레이어가 된 칸은 모두 켬
	E_EXPECT_TRUE(Layers.ShouldCollide(1, 3) && Layers.ShouldCollide(2, 3));
	E_EXPECT_EQ(Layers.ResolveLayer("Ghost"), 0u);

	uint32      Mask = 0;
	std::string Unknown;
	E_EXPECT_TRUE(Layers.MakeMask({ "Ground", "Prop" }, Mask, &Unknown));
	E_EXPECT_EQ(Mask, (1u << 1) | (1u << 2));
	E_EXPECT_FALSE(Layers.MakeMask({ "Ground", "Nope" }, Mask, &Unknown));
	E_EXPECT_TRUE(Unknown == "Nope");
}

// JSON: 칸 순서 이름 + 꺼진 쌍(이름). 왕복 동일, 첫 칸은 항상 Default, 중복 이름/없는 쌍은 건너뜀
E_TEST(CollisionLayers_Json)
{
	const FCollisionLayerSettings Source = MakeTestLayers();
	FCollisionLayerSettings       Loaded;
	E_EXPECT_TRUE(Loaded.FromJson(Source.ToJson()));
	E_EXPECT_TRUE(Loaded.ToJson() == Source.ToJson());
	for (uint32 A = 0; A < FCollisionLayerSettings::MaxLayers; ++A)
	{
		E_EXPECT_EQ(Loaded.GetCollisionMask(A), Source.GetCollisionMask(A));
	}

	std::string Error;
	E_EXPECT_TRUE(Loaded.FromJson(R"({ "Layers": ["Wrong", "A", "A", "B"], "DisabledPairs": [["A", "B"], ["A", "Missing"]] })", &Error));
	E_EXPECT_TRUE(Loaded.GetLayerName(0) == "Default");
	E_EXPECT_TRUE(Loaded.GetLayerName(1) == "A" && Loaded.GetLayerName(2).empty() && Loaded.GetLayerName(3) == "B");
	E_EXPECT_FALSE(Loaded.ShouldCollide(1, 3));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_FALSE(Loaded.FromJson("not json"));
	E_EXPECT_TRUE(Loaded.FromJson("{}")); // 빈 파일 = 기본값
	E_EXPECT_EQ(Loaded.GetLayerNames().size(), size_t(1));
}

// 행렬에서 꺼진 레이어끼리는 통과, 켜진 쌍과 이름 없는(Default) 바디는 예전처럼 부딪힌다
E_TEST(CollisionLayers_BodiesPassThroughDisabledPairs)
{
	const FScopedCollisionLayers Scoped(MakeTestLayers());
	FScene                       Scene;
	AddBox(Scene, "Floor", FVector3(0.0f, 0.0f, -10.0f), FVector3(1000.0f, 1000.0f, 10.0f), EPhysicsMotionType::Static, "Ground");
	const FEntity Prop    = AddBall(Scene, "Prop", FVector3(-200.0f, 0.0f, 100.0f), "Prop");
	const FEntity Ghost   = AddBall(Scene, "Ghost", FVector3(0.0f, 0.0f, 100.0f), "Ghost");
	const FEntity Plain   = AddBall(Scene, "Plain", FVector3(200.0f, 0.0f, 100.0f), "");
	const FEntity Unknown = AddBall(Scene, "Unknown", FVector3(400.0f, 0.0f, 100.0f), "NoSuchLayer"); // 경고 + Default
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Simulate(Physics, Scene, 1.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Prop).Position.Z, 25.0f, 2.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Plain).Position.Z, 25.0f, 2.0f);
	E_EXPECT_NEAR(Scene.GetTransform(Unknown).Position.Z, 25.0f, 2.0f);
	E_EXPECT_TRUE(Scene.GetTransform(Ghost).Position.Z < -100.0f); // 바닥을 뚫고 떨어짐

	// 레이어를 바꾸면 바디를 다시 만든다 (스크립트/인스펙터)
	Scene.GetRegistry().Get<FSphereColliderComponent>(Prop).Layer = "Ghost";
	Simulate(Physics, Scene, 0.5f);
	E_EXPECT_TRUE(Scene.GetTransform(Prop).Position.Z < 0.0f);
	Physics.End();
}

// 레이캐스트 마스크: 고른 레이어만 맞는다. 기본은 전체 (트리거는 항상 제외)
E_TEST(CollisionLayers_RaycastMask)
{
	const FScopedCollisionLayers Scoped(MakeTestLayers());
	FScene                       Scene;
	const FEntity Floor = AddBox(Scene, "Floor", FVector3(0.0f, 0.0f, -10.0f), FVector3(1000.0f, 1000.0f, 10.0f), EPhysicsMotionType::Static, "Ground");
	const FEntity Crate = AddBox(Scene, "Crate", FVector3(0.0f, 0.0f, 50.0f), FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, "Prop");
	const FEntity Zone  = AddBox(Scene, "Zone", FVector3(0.0f, 0.0f, 300.0f), FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, "Prop");
	Scene.GetRegistry().Get<FBoxColliderComponent>(Zone).bIsTrigger = true;
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, 0.0f);
	const FVector3 Origin(0.0f, 0.0f, 500.0f);
	const FVector3 Down(0.0f, 0.0f, -1.0f);
	const FCollisionLayerSettings& Layers = FProjectSettings::Get().Collision;

	FPhysicsHit Hit;
	E_EXPECT_TRUE(Physics.Raycast(Origin, Down, 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == Crate); // 트리거(Zone)는 지나친다
	uint32 Mask = 0;
	E_EXPECT_TRUE(Layers.MakeMask({ "Ground" }, Mask));
	E_EXPECT_TRUE(Physics.Raycast(Origin, Down, 1000.0f, Hit, Mask));
	E_EXPECT_TRUE(Hit.Entity == Floor);
	E_EXPECT_NEAR(Hit.Distance, 500.0f, 0.5f);
	E_EXPECT_TRUE(Layers.MakeMask({ "Ghost", "Default" }, Mask));
	E_EXPECT_FALSE(Physics.Raycast(Origin, Down, 1000.0f, Hit, Mask));
	Physics.End();
}

// 캐릭터: 자기 레이어로 막힘이 정해진다 (Character × Prop 꺼짐 → 상자 통과, Default 벽은 막힘), Character × Character 꺼짐 → 서로 통과
E_TEST(CollisionLayers_CharacterUsesMatrix)
{
	const FScopedCollisionLayers Scoped(MakeTestLayers());
	FScene                       Scene;
	AddBox(Scene, "Floor", FVector3(0.0f, 0.0f, -10.0f), FVector3(3000.0f, 3000.0f, 10.0f), EPhysicsMotionType::Static, "Ground");
	AddBox(Scene, "PropWall", FVector3(150.0f, 0.0f, 100.0f), FVector3(10.0f, 200.0f, 100.0f), EPhysicsMotionType::Static, "Prop");
	AddBox(Scene, "Wall", FVector3(150.0f, 600.0f, 100.0f), FVector3(10.0f, 200.0f, 100.0f), EPhysicsMotionType::Static, "");
	const FEntity Through = AddCharacter(Scene, "Through", FVector3(0.0f, 0.0f, StandingZ + 1.0f), "Character");
	const FEntity Blocked = AddCharacter(Scene, "Blocked", FVector3(0.0f, 600.0f, StandingZ + 1.0f), "Character");
	// 마주 보고 걷는 두 캐릭터 (Character 레이어끼리 통과) / Default 두 캐릭터 (막힘)
	const FEntity GhostLeft  = AddCharacter(Scene, "GhostLeft", FVector3(-200.0f, 1200.0f, StandingZ + 1.0f), "Character");
	const FEntity GhostRight = AddCharacter(Scene, "GhostRight", FVector3(200.0f, 1200.0f, StandingZ + 1.0f), "Character");
	const FEntity SolidLeft  = AddCharacter(Scene, "SolidLeft", FVector3(-200.0f, 1800.0f, StandingZ + 1.0f), "");
	const FEntity SolidRight = AddCharacter(Scene, "SolidRight", FVector3(200.0f, 1800.0f, StandingZ + 1.0f), "");
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	Physics.Update(Scene, Frame);
	Scene.UpdateTransforms();
	const FVector2 Right(1.0f, 0.0f);
	const FVector2 Left(-1.0f, 0.0f);
	WalkCharacters(Physics, Scene, { { Through, Right }, { Blocked, Right }, { GhostLeft, Right }, { GhostRight, Left }, { SolidLeft, Right }, { SolidRight, Left } },
	               1.5f);
	E_EXPECT_TRUE(Scene.GetTransform(Through).Position.X > 300.0f);           // Prop 벽 통과
	E_EXPECT_TRUE(Scene.GetTransform(Blocked).Position.X < 150.0f - 35.0f + 2.0f); // Default 벽에 막힘
	E_EXPECT_TRUE(Scene.GetTransform(GhostLeft).Position.X > Scene.GetTransform(GhostRight).Position.X); // 엇갈려 지나감
	E_EXPECT_TRUE(Scene.GetTransform(SolidRight).Position.X - Scene.GetTransform(SolidLeft).Position.X > 2.0f * 35.0f - 5.0f);
	Physics.End();
}

// 트리거도 행렬을 따른다: Sensor × Prop 꺼짐 → 알림 없음, Default 공은 들어옴 알림
E_TEST(CollisionLayers_TriggerRespectsMatrix)
{
	const FScopedCollisionLayers Scoped(MakeTestLayers());
	FScene                       Scene;
	const FEntity Zone = AddBox(Scene, "Zone", FVector3(0.0f, 0.0f, 0.0f), FVector3(400.0f, 400.0f, 50.0f), EPhysicsMotionType::Static, "Sensor");
	Scene.GetRegistry().Get<FBoxColliderComponent>(Zone).bIsTrigger = true;
	const FEntity Prop  = AddBall(Scene, "Prop", FVector3(-100.0f, 0.0f, 150.0f), "Prop");
	const FEntity Plain = AddBall(Scene, "Plain", FVector3(100.0f, 0.0f, 150.0f), "");
	Scene.UpdateTransforms();

	FPhysicsSystem Physics;
	Physics.Begin();
	std::vector<FCollisionEvent> Events;
	Simulate(Physics, Scene, 0.6f, &Events);
	E_EXPECT_EQ(CountEvents(Events, ECollisionEventType::TriggerEnter, Plain), 1);
	E_EXPECT_EQ(CountEvents(Events, ECollisionEventType::TriggerEnter, Prop), 0);
	E_EXPECT_TRUE(Scene.GetTransform(Prop).Position.Z < 0.0f); // 트리거는 막지 않는다
	Physics.End();
}

// Lua Physics.Raycast(origin, dir, dist, layers): 이름 표/이름 하나, 생략 = 전체, 없는 이름은 오류
E_TEST(CollisionLayers_LuaRaycastLayers)
{
	const FScopedCollisionLayers Scoped(MakeTestLayers());
	FScene                       Scene;
	AddBox(Scene, "Floor", FVector3(0.0f, 0.0f, -10.0f), FVector3(1000.0f, 1000.0f, 10.0f), EPhysicsMotionType::Static, "Ground");
	AddBox(Scene, "Crate", FVector3(0.0f, 0.0f, 50.0f), FVector3(50.0f, 50.0f, 50.0f), EPhysicsMotionType::Static, "Prop");
	Scene.UpdateTransforms();

	const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectECollisionLayerTests";
	std::filesystem::create_directories(Content);
	FScriptSystem   Scripts;
	FPhysicsSystem  Physics;
	FGameModuleHost Host;
	FGameWorld      World;
	World.Init({ &Scripts, &Physics, &Host, nullptr, Content });
	World.BeginPlay(Scene);
	World.TickGameplay(Frame, nullptr);
	E_EXPECT_TRUE(Scripts.RunString(R"(
local Origin, Down = Vector3(0, 0, 500), Vector3(0, 0, -1)
assert(Physics.Raycast(Origin, Down, 1000).entity:GetName() == 'Crate')
assert(Physics.Raycast(Origin, Down, 1000, {'Ground'}).entity:GetName() == 'Floor')
assert(Physics.Raycast(Origin, Down, 1000, 'Prop').entity:GetName() == 'Crate')
assert(Physics.Raycast(Origin, Down, 1000, {'Ghost'}) == nil)
assert(not pcall(Physics.Raycast, Origin, Down, 1000, {'NoSuchLayer'}))
)"));
	World.EndPlay();
}

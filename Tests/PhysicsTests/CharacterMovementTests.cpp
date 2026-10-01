#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

// 캐릭터 이동 (FCharacterMovementComponent + Jolt CharacterVirtual). 위치 = 캡슐 중심, 바닥에 서면 z = 반지름 35 + 원기둥 절반 55 = 90
namespace
{
	constexpr float Frame        = 1.0f / 60.0f;
	constexpr float StandingZ    = 90.0f;

	FEntity AddStaticBox(FScene& Scene, const char* Name, const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation = FQuat())
	{
		const FEntity Box = Scene.CreateEntity(Name);
		Scene.GetTransform(Box).Position = Center;
		Scene.GetTransform(Box).Rotation = Rotation;
		Scene.GetRegistry().Emplace<FBoxColliderComponent>(Box).HalfExtents = HalfExtents;
		Scene.GetRegistry().Emplace<FRigidBodyComponent>(Box).MotionType   = static_cast<int32>(EPhysicsMotionType::Static);
		return Box;
	}

	FEntity AddFloor(FScene& Scene)
	{
		return AddStaticBox(Scene, "Floor", FVector3(0.0f, 0.0f, -10.0f), FVector3(3000.0f, 3000.0f, 10.0f)); // 윗면 z = 0
	}

	FEntity AddCharacter(FScene& Scene, const char* Name, const FVector3& Position)
	{
		const FEntity Character = Scene.CreateEntity(Name);
		Scene.GetTransform(Character).Position = Position;
		Scene.GetRegistry().Emplace<FCharacterMovementComponent>(Character);
		return Character;
	}

	struct FCharacterWorld
	{
		FScene         Scene;
		FPhysicsSystem Physics;

		void Begin()
		{
			Scene.UpdateTransforms();
			Physics.Begin();
			Physics.Update(Scene, Frame); // 바디/캐릭터 생성
			Scene.UpdateTransforms();
		}

		// 캐릭터들에 같은 입력을 Seconds 동안 (무브마다 물리 한 프레임)
		void Run(std::initializer_list<FEntity> Characters, float Seconds, const FVector2& Input, bool bJump = false, float Yaw = 0.0f)
		{
			const int32 Frames = static_cast<int32>(std::lround(Seconds / Frame));
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				for (const FEntity Character : Characters)
				{
					FCharacterMove Move;
					Move.DeltaSeconds = Frame;
					Move.Input        = Input;
					Move.bJump        = bJump && Index == 0;
					Move.Yaw          = Yaw;
					Physics.SimulateCharacter(Scene, Character, Move);
				}
				Physics.Update(Scene, Frame);
				Scene.UpdateTransforms();
			}
		}

		FVector3 Position(FEntity Entity) { return Scene.GetTransform(Entity).Position; }
	};
} // namespace

E_TEST(CharacterMath_VelocityRules)
{
	FCharacterMovementComponent Movement; // 450 cm/s, 점프 520
	FCharacterMove              Move;
	Move.DeltaSeconds = 0.1f;
	Move.Input        = FVector2(3.0f, 4.0f); // 길이 5 → 1로 잘린다
	bool bJumped      = false;

	// 바닥: 수평 = 입력 × 속도, 수직 0 + 중력 × dt
	FVector3 Velocity = CharacterMovementMath::ComputeVelocity(Movement, FVector3(0.0f, 0.0f, -50.0f), true, Move, -980.0f, bJumped);
	E_EXPECT_NEAR(Velocity.X, 270.0f, 1.0e-3f);
	E_EXPECT_NEAR(Velocity.Y, 360.0f, 1.0e-3f);
	E_EXPECT_NEAR(Velocity.Z, -98.0f, 1.0e-3f);
	E_EXPECT_FALSE(bJumped);

	// 점프는 바닥에서만
	Move.bJump = true;
	Velocity   = CharacterMovementMath::ComputeVelocity(Movement, FVector3(), true, Move, -980.0f, bJumped);
	E_EXPECT_TRUE(bJumped);
	E_EXPECT_NEAR(Velocity.Z, 520.0f - 98.0f, 1.0e-3f);
	Velocity = CharacterMovementMath::ComputeVelocity(Movement, FVector3(0.0f, 0.0f, 100.0f), false, Move, -980.0f, bJumped);
	E_EXPECT_FALSE(bJumped);
	E_EXPECT_NEAR(Velocity.Z, 2.0f, 1.0e-3f);

	// 공중: AirControl 0.35 × 450 × 4 × 0.1 = 63 cm/s 만큼만 입력 쪽으로
	Move.bJump  = false;
	Move.Input  = FVector2(1.0f, 0.0f);
	Velocity    = CharacterMovementMath::ComputeVelocity(Movement, FVector3(0.0f, 0.0f, 0.0f), false, Move, -980.0f, bJumped);
	E_EXPECT_NEAR(Velocity.X, 63.0f, 1.0e-3f);
	E_EXPECT_NEAR(Velocity.Y, 0.0f, 1.0e-3f);

	// dt 상한 (0.1초)
	Move.DeltaSeconds = 5.0f;
	Velocity          = CharacterMovementMath::ComputeVelocity(Movement, FVector3(), true, Move, -980.0f, bJumped);
	E_EXPECT_NEAR(Velocity.Z, -98.0f, 1.0e-3f);
	E_EXPECT_TRUE(CharacterMovementMath::ClampInput(FVector2(0.2f, 0.0f)).X == 0.2f); // 1보다 짧으면 그대로 (아날로그)
}

E_TEST(Character_LandsWalksAndFaces)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	const FEntity Character = AddCharacter(World.Scene, "Character", FVector3(0.0f, 0.0f, 200.0f));
	World.Begin();
	E_EXPECT_TRUE(World.Physics.HasCharacter(Character));
	E_EXPECT_FALSE(World.Physics.HasBody(Character)); // 바디가 아니라 캐릭터

	World.Run({ Character }, 1.5f, FVector2(0.0f, 0.0f));
	E_EXPECT_NEAR(World.Position(Character).Z, StandingZ, 2.0f);
	E_EXPECT_TRUE(World.Physics.IsGrounded(Character));

	World.Run({ Character }, 1.0f, FVector2(1.0f, 0.0f), false, 90.0f);
	E_EXPECT_NEAR(World.Position(Character).X, 450.0f, 15.0f);
	E_EXPECT_NEAR(World.Position(Character).Z, StandingZ, 2.0f);
	// 몸 방향 = 무브 yaw (90도 = +Y를 본다)
	E_EXPECT_EQUALS(World.Scene.GetTransform(Character).Rotation.GetForwardVector(), FVector3::RightVector, 1.0e-3f);
	E_EXPECT_NEAR(World.Physics.GetVelocity(Character).X, 450.0f, 5.0f);
}

E_TEST(Character_SlidesAlongWall)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	AddStaticBox(World.Scene, "Wall", FVector3(200.0f, 0.0f, 150.0f), FVector3(20.0f, 1000.0f, 150.0f)); // x 180~220
	const FEntity Character = AddCharacter(World.Scene, "Character", FVector3(0.0f, 0.0f, StandingZ + 1.0f));
	World.Begin();
	World.Run({ Character }, 1.5f, FVector2(0.7071f, 0.7071f));
	const FVector3 Position = World.Position(Character);
	E_EXPECT_TRUE(Position.X < 180.0f - 34.0f && Position.X > 100.0f); // 벽에 막혀 멈춤
	E_EXPECT_TRUE(Position.Y > 300.0f);                                 // 벽을 따라 미끄러짐
}

E_TEST(Character_ClimbsStepButNotLedge)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	AddStaticBox(World.Scene, "Step", FVector3(500.0f, 0.0f, 15.0f), FVector3(300.0f, 100.0f, 15.0f));      // 높이 30 (계단 35 이하), x 200~800
	AddStaticBox(World.Scene, "Ledge", FVector3(500.0f, 600.0f, 30.0f), FVector3(300.0f, 100.0f, 30.0f));   // 높이 60
	const FEntity Climber = AddCharacter(World.Scene, "Climber", FVector3(0.0f, 0.0f, StandingZ + 1.0f));
	const FEntity Blocked = AddCharacter(World.Scene, "Blocked", FVector3(0.0f, 600.0f, StandingZ + 1.0f));
	World.Begin();
	World.Run({ Climber, Blocked }, 1.0f, FVector2(1.0f, 0.0f));
	E_EXPECT_NEAR(World.Position(Climber).Z, StandingZ + 30.0f, 3.0f); // 계단 위
	E_EXPECT_TRUE(World.Position(Climber).X > 250.0f);
	E_EXPECT_NEAR(World.Position(Blocked).Z, StandingZ, 3.0f);         // 못 올라감
	E_EXPECT_TRUE(World.Position(Blocked).X < 200.0f - 30.0f);
}

E_TEST(Character_SlopeLimit)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	// 경사면: 회전한 판 (윗면이 기울어짐). 30도는 오르고 70도는 못 오른다 (최대 경사 50도)
	const auto AddRamp = [&World](const char* Name, float Y, float Degrees) {
		const float Radians = FMath::DegreesToRadians(Degrees);
		const float Length  = 600.0f;
		// 판의 아래쪽 앞 모서리가 x = 200, z = 0에 오도록 중심을 둔다 (+X로 올라가는 면)
		const FVector3 Center(200.0f + 0.5f * Length * std::cos(Radians), Y, 0.5f * Length * std::sin(Radians) - 10.0f * std::cos(Radians));
		AddStaticBox(World.Scene, Name, Center, FVector3(0.5f * Length, 100.0f, 10.0f), FQuat::FromEuler(Degrees, 0.0f, 0.0f));
	};
	AddRamp("Gentle", 0.0f, 30.0f);
	AddRamp("Steep", 600.0f, 70.0f);
	const FEntity Up    = AddCharacter(World.Scene, "Up", FVector3(0.0f, 0.0f, StandingZ + 1.0f));
	const FEntity Stuck = AddCharacter(World.Scene, "Stuck", FVector3(0.0f, 600.0f, StandingZ + 1.0f));
	World.Begin();
	World.Run({ Up, Stuck }, 2.0f, FVector2(1.0f, 0.0f));
	E_EXPECT_TRUE(World.Position(Up).Z > StandingZ + 100.0f); // 30도: 오른다
	E_EXPECT_TRUE(World.Position(Stuck).Z < StandingZ + 60.0f); // 70도: 거의 못 오른다
}

E_TEST(Character_JumpHeightAndLanding)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	const FEntity Character = AddCharacter(World.Scene, "Character", FVector3(0.0f, 0.0f, StandingZ + 1.0f));
	World.Begin();
	World.Run({ Character }, 0.5f, FVector2(0.0f, 0.0f));
	float Peak = 0.0f;
	for (int32 Step = 0; Step < 90; ++Step)
	{
		World.Run({ Character }, Frame, FVector2(0.0f, 0.0f), Step == 0);
		Peak = std::max(Peak, World.Position(Character).Z - StandingZ);
	}
	// v² / 2g = 520² / (2 × 980.665) ≈ 138cm
	E_EXPECT_NEAR(Peak, 138.0f, 8.0f);
	E_EXPECT_NEAR(World.Position(Character).Z, StandingZ, 2.0f);
	E_EXPECT_TRUE(World.Physics.IsGrounded(Character));
}

E_TEST(Character_PushesBoxesAndBlocksCharacters)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	const FEntity Box = World.Scene.CreateEntity("Box");
	World.Scene.GetTransform(Box).Position = FVector3(150.0f, 0.0f, 30.0f);
	World.Scene.GetRegistry().Emplace<FBoxColliderComponent>(Box).HalfExtents = FVector3(30.0f, 30.0f, 30.0f);
	World.Scene.GetRegistry().Emplace<FRigidBodyComponent>(Box).Mass          = 20.0f;
	const FEntity Pusher = AddCharacter(World.Scene, "Pusher", FVector3(0.0f, 0.0f, StandingZ + 1.0f));
	// 마주 보고 걷는 두 캐릭터 (y = 600 줄)
	const FEntity Left  = AddCharacter(World.Scene, "Left", FVector3(-200.0f, 600.0f, StandingZ + 1.0f));
	const FEntity Right = AddCharacter(World.Scene, "Right", FVector3(200.0f, 600.0f, StandingZ + 1.0f));
	World.Begin();

	World.Run({ Pusher }, 1.5f, FVector2(1.0f, 0.0f));
	E_EXPECT_TRUE(World.Position(Box).X > 200.0f); // 밀렸다

	for (int32 Step = 0; Step < 90; ++Step)
	{
		World.Run({ Left }, Frame, FVector2(1.0f, 0.0f));
		World.Run({ Right }, Frame, FVector2(-1.0f, 0.0f));
	}
	const float Gap = World.Position(Right).X - World.Position(Left).X;
	E_EXPECT_TRUE(Gap > 2.0f * 35.0f - 5.0f); // 서로 통과하지 않는다
}

// 물리 예측용 API: 캐릭터 밀기 끄기(재조정 다시 적용), 접촉 목록, 동적 바디 상태 읽기/설정/보정
E_TEST(Character_PushToggleContactsAndBodyCorrection)
{
	FCharacterWorld World;
	AddFloor(World.Scene);
	const FEntity Pusher = AddCharacter(World.Scene, "Pusher", FVector3(0.0f, 0.0f, StandingZ + 1.0f));
	const FEntity Box    = World.Scene.CreateEntity("Box");
	World.Scene.GetTransform(Box).Position                          = FVector3(110.0f, 0.0f, 30.0f);
	World.Scene.GetRegistry().Emplace<FBoxColliderComponent>(Box).HalfExtents = FVector3(30.0f, 30.0f, 30.0f);
	World.Scene.GetRegistry().Emplace<FRigidBodyComponent>(Box).MotionType    = static_cast<int32>(EPhysicsMotionType::Dynamic);
	World.Begin();
	World.Run({ Pusher }, 0.5f, FVector2()); // 착지/안정
	E_EXPECT_TRUE(World.Physics.IsDynamicBody(Box));

	// 밀기 끔: 동적 상자도 벽처럼 막고 움직이지 않는다 + 접촉 목록에 나온다
	World.Physics.SetCharactersPushBodies(false);
	World.Run({ Pusher }, 0.5f, FVector2(1.0f, 0.0f));
	E_EXPECT_NEAR(World.Position(Box).X, 110.0f, 1.0f);
	E_EXPECT_NEAR(World.Position(Pusher).X, 110.0f - 30.0f - 35.0f, 3.0f);
	std::vector<FEntity> Contacts;
	World.Physics.GetCharacterContacts(Pusher, Contacts);
	E_EXPECT_TRUE(std::find(Contacts.begin(), Contacts.end(), Box) != Contacts.end());
	// 밀기 켬: 밀린다
	World.Physics.SetCharactersPushBodies(true);
	World.Run({ Pusher }, 0.5f, FVector2(1.0f, 0.0f));
	E_EXPECT_TRUE(World.Position(Box).X > 140.0f);
	World.Run({ Pusher }, 1.0f, FVector2()); // 멈춤

	// 상태 설정 (순간이동 + 속도) / 보정 (위치·속도에 더함)
	FPhysicsBodyMotion Motion;
	E_EXPECT_TRUE(World.Physics.GetBodyMotion(Box, Motion));
	Motion.Position       = FVector3(500.0f, 500.0f, 30.0f);
	Motion.LinearVelocity = FVector3(100.0f, 0.0f, 0.0f);
	World.Physics.SetBodyMotion(Box, Motion);
	FPhysicsBodyMotion After;
	E_EXPECT_TRUE(World.Physics.GetBodyMotion(Box, After));
	E_EXPECT_NEAR(After.Position.X, 500.0f, 0.01f);
	E_EXPECT_NEAR(After.LinearVelocity.X, 100.0f, 0.5f);
	World.Physics.CorrectBody(Box, FVector3(0.0f, 10.0f, 0.0f), FQuat(), FVector3(0.0f, 300.0f, 0.0f), FVector3());
	E_EXPECT_TRUE(World.Physics.GetBodyMotion(Box, After));
	E_EXPECT_NEAR(After.Position.Y, 510.0f, 0.01f);
	E_EXPECT_NEAR(After.LinearVelocity.Y, 300.0f, 0.5f);
	World.Run({}, 0.1f, FVector2());
	E_EXPECT_TRUE(World.Physics.GetBodyMotion(Box, After));
	E_EXPECT_TRUE(After.Position.Y > 530.0f);       // 보정한 속도로 이어서 움직인다 (바닥 마찰로 줄면서)
	E_EXPECT_TRUE(World.Position(Box).Y > 510.0f); // 화면도 보정 위치에서 이어진다
	E_EXPECT_FALSE(World.Physics.GetBodyMotion(Pusher, After)); // 캐릭터는 바디가 아니다
}
// 2D 캐릭터 이동기 (Physics/CharacterMovement2D.h): 순수 이동 수학(CharacterMovement2D_*)과 실제 2D 월드 이동(Character2DWorld_*).
// 테스트는 Box2D를 직접 부르지 않고 엔진 래퍼(FPhysics2DSystem + FCharacterMovement2DSystem)만 쓴다
#include "Core/Testing/TestFramework.h"
#include "Physics/CharacterMovement2D.h"
#include "Physics/CharacterMovement2DSystem.h"
#include "Physics/Physics2DComponents.h"
#include "Physics/Physics2DSystem.h"
#include "Physics/PhysicsWorld.h" // LogPhysics
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <format>
#include <utility>
#include <vector>

namespace
{
	constexpr float Frame   = 1.0f / 60.0f;
	constexpr float Gravity = -980.665f; // 프로젝트 설정 기본 Gravity2D Z

	FCharacterMove2D MakeMove(float InputX = 0.0f, bool bJump = false, bool bHeld = false)
	{
		FCharacterMove2D Move;
		Move.DeltaSeconds = Frame;
		Move.Input        = FVector2(InputX, 0.0f);
		Move.bJumpPressed = bJump;
		Move.bJumpHeld    = bHeld || bJump;
		return Move;
	}

	// 충돌 없이 수학만으로 한 무브 (바닥 여부는 호출자가 정한다)
	FCharacterMove2DEvents Step(const FCharacterMovement2DComponent& Movement, FCharacterState2D& State, const FCharacterMove2D& Move, bool bGroundedAfter)
	{
		FCharacterMove2DEvents Events;
		CharacterMovement2DMath::BeginMove(Movement, State, Move, Gravity, FVector2(), Events);
		State.Position = State.Position + State.Velocity * Move.DeltaSeconds;
		CharacterMovement2DMath::EndMove(Movement, State, bGroundedAfter && State.Velocity.Y <= 0.0f, Events);
		return Events;
	}

	FCharacterState2D Grounded()
	{
		FCharacterState2D State;
		State.bGrounded = true;
		return State;
	}
} // namespace

// ---------------------------------------------------------------- 순수 수학

E_TEST(CharacterMovement2D_AccelerationAndDeceleration)
{
	FCharacterMovement2DComponent Movement;
	FCharacterState2D             State = Grounded();
	Step(Movement, State, MakeMove(1.0f), true);
	E_EXPECT_NEAR(State.Velocity.X, Movement.GroundAcceleration * Frame, 1.0e-3f);
	for (int32 Index = 0; Index < 30; ++Index)
	{
		Step(Movement, State, MakeMove(1.0f), true);
	}
	E_EXPECT_NEAR(State.Velocity.X, Movement.MaxSpeed, 1.0e-3f); // 최대 속력에서 멈춘다
	Step(Movement, State, MakeMove(0.0f), true);
	E_EXPECT_NEAR(State.Velocity.X, Movement.MaxSpeed - Movement.GroundDeceleration * Frame, 1.0e-3f);
	// 반대 입력은 max(가속, 감속)
	State.Velocity.X = 300.0f;
	Step(Movement, State, MakeMove(-1.0f), true);
	E_EXPECT_NEAR(State.Velocity.X, 300.0f - std::max(Movement.GroundAcceleration, Movement.GroundDeceleration) * Frame, 1.0e-3f);
	// 공중은 공중 가속
	FCharacterState2D Air;
	Step(Movement, Air, MakeMove(1.0f), false);
	E_EXPECT_NEAR(Air.Velocity.X, Movement.AirAcceleration * Frame, 1.0e-3f);
	// 플랫포머는 입력 Y를 버린다
	E_EXPECT_NEAR(CharacterMovement2DMath::ClampInput(FVector2(0.5f, 1.0f), ECharacterMovement2DMode::Platformer).Y, 0.0f, 0.0f);
}

E_TEST(CharacterMovement2D_JumpHeight)
{
	FCharacterMovement2DComponent Movement;
	Movement.MaxJumps        = 1;
	const float       Height = CharacterMovement2DMath::ComputeJumpHeight(Movement, Gravity);
	E_EXPECT_NEAR(Height, Movement.JumpVelocity * Movement.JumpVelocity / (2.0f * 980.665f * Movement.GravityScale), 1.0e-2f);
	FCharacterState2D State  = Grounded();
	FCharacterMove2DEvents Events = Step(Movement, State, MakeMove(0.0f, true), true);
	E_EXPECT_TRUE(Events.bJumped);
	E_EXPECT_EQ(Events.JumpIndex, 1);
	float MaxZ = State.Position.Y;
	for (int32 Index = 0; Index < 120 && !State.bGrounded; ++Index)
	{
		Step(Movement, State, MakeMove(0.0f, false, true), State.Position.Y <= 0.0f);
		MaxZ = std::max(MaxZ, State.Position.Y);
	}
	E_EXPECT_NEAR(MaxZ, Height, Movement.JumpVelocity * Frame); // 반암시 오일러 오차 한 스텝 안
	E_EXPECT_TRUE(State.bGrounded);
}

E_TEST(CharacterMovement2D_DoubleJumpAndFallOffCountsFirstJump)
{
	FCharacterMovement2DComponent Movement; // MaxJumps 2
	FCharacterState2D             State = Grounded();
	E_EXPECT_EQ(Step(Movement, State, MakeMove(0.0f, true), true).JumpIndex, 1);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Step(Movement, State, MakeMove(0.0f, false, true), false);
	}
	const FCharacterMove2DEvents Second = Step(Movement, State, MakeMove(0.0f, true), false);
	E_EXPECT_TRUE(Second.bJumped);
	E_EXPECT_EQ(Second.JumpIndex, 2);
	E_EXPECT_NEAR(State.Velocity.Y, Movement.JumpVelocity + Gravity * Movement.GravityScale * Frame, 1.0e-2f);
	Step(Movement, State, MakeMove(0.0f, false, false), false);
	E_EXPECT_FALSE(Step(Movement, State, MakeMove(0.0f, true), false).bJumped); // 점프 다 씀

	// 걸어서 떨어지고 코요테 시간이 지나면 첫 점프를 쓴 것으로 친다 → 남은 점프 1 (공중 점프 = 2번째)
	FCharacterState2D Walk = Grounded();
	Step(Movement, Walk, MakeMove(), false); // 바닥을 떠남
	for (int32 Index = 0; Index < 12; ++Index) // 0.2초 > 코요테 0.1초
	{
		Step(Movement, Walk, MakeMove(), false);
	}
	E_EXPECT_EQ(static_cast<int32>(Walk.JumpsUsed), 1);
	const FCharacterMove2DEvents Air = Step(Movement, Walk, MakeMove(0.0f, true), false);
	E_EXPECT_TRUE(Air.bJumped);
	E_EXPECT_EQ(Air.JumpIndex, 2);
}

E_TEST(CharacterMovement2D_CoyoteTime)
{
	FCharacterMovement2DComponent Movement;
	Movement.MaxJumps       = 1;
	FCharacterState2D State = Grounded();
	Step(Movement, State, MakeMove(), false); // 이 무브 끝에 바닥을 떠남
	for (int32 Index = 0; Index < 3; ++Index) // 0.05초 — 코요테 0.1초 안
	{
		Step(Movement, State, MakeMove(), false);
	}
	const FCharacterMove2DEvents Late = Step(Movement, State, MakeMove(0.0f, true), false);
	E_EXPECT_TRUE(Late.bJumped); // 바닥 점프로 허용
	E_EXPECT_EQ(Late.JumpIndex, 1);

	FCharacterState2D Expired = Grounded();
	Step(Movement, Expired, MakeMove(), false);
	for (int32 Index = 0; Index < 9; ++Index) // 0.15초
	{
		Step(Movement, Expired, MakeMove(), false);
	}
	E_EXPECT_FALSE(Step(Movement, Expired, MakeMove(0.0f, true), false).bJumped);
}

E_TEST(CharacterMovement2D_JumpBuffer)
{
	FCharacterMovement2DComponent Movement;
	Movement.MaxJumps = 1;
	// 공중(점프 다 씀)에서 누르고 0.05초 뒤 착지 → 착지 다음 무브에 점프
	FCharacterState2D State;
	State.JumpsUsed = 1;
	E_EXPECT_FALSE(Step(Movement, State, MakeMove(0.0f, true), false).bJumped);
	Step(Movement, State, MakeMove(0.0f, false, true), false);
	Step(Movement, State, MakeMove(0.0f, false, true), true); // 착지
	E_EXPECT_TRUE(State.bGrounded);
	E_EXPECT_TRUE(Step(Movement, State, MakeMove(0.0f, false, true), true).bJumped);

	// 버퍼가 지나면 없음
	FCharacterState2D Old;
	Old.JumpsUsed = 1;
	Step(Movement, Old, MakeMove(0.0f, true), false);
	for (int32 Index = 0; Index < 9; ++Index)
	{
		Step(Movement, Old, MakeMove(0.0f, false, true), false);
	}
	Step(Movement, Old, MakeMove(0.0f, false, true), true);
	E_EXPECT_FALSE(Step(Movement, Old, MakeMove(0.0f, false, true), true).bJumped);
}

E_TEST(CharacterMovement2D_VariableJumpCut)
{
	FCharacterMovement2DComponent Movement;
	FCharacterState2D             Held = Grounded();
	FCharacterState2D             Cut  = Grounded();
	Step(Movement, Held, MakeMove(0.0f, true), true);
	Step(Movement, Cut, MakeMove(0.0f, true), true);
	const float Before = Cut.Velocity.Y;
	Step(Movement, Held, MakeMove(0.0f, false, true), false);
	Step(Movement, Cut, MakeMove(0.0f, false, false), false); // 뗌
	E_EXPECT_NEAR(Cut.Velocity.Y, (Before * Movement.JumpCutFactor) + Gravity * Movement.GravityScale * Frame, 1.0e-2f);
	E_EXPECT_TRUE(Held.Velocity.Y > Cut.Velocity.Y + 400.0f);
	// 컷은 한 번 — 다시 떼어 있어도 더 줄지 않는다
	const float AfterCut = Cut.Velocity.Y;
	Step(Movement, Cut, MakeMove(0.0f, false, false), false);
	E_EXPECT_NEAR(Cut.Velocity.Y, AfterCut + Gravity * Movement.GravityScale * Frame, 1.0e-2f);
	// 높이 비교: 짧게 누른 점프가 낮다
	float HeldMax = Held.Position.Y, CutMax = Cut.Position.Y;
	for (int32 Index = 0; Index < 90; ++Index)
	{
		Step(Movement, Held, MakeMove(0.0f, false, true), false);
		Step(Movement, Cut, MakeMove(0.0f, false, false), false);
		HeldMax = std::max(HeldMax, Held.Position.Y);
		CutMax  = std::max(CutMax, Cut.Position.Y);
	}
	E_EXPECT_TRUE(CutMax < HeldMax * 0.5f);
}

E_TEST(CharacterMovement2D_Dash)
{
	FCharacterMovement2DComponent Movement;
	FCharacterState2D             State = Grounded();
	FCharacterMove2D              Move  = MakeMove();
	Move.bDash         = true;
	Move.DashDirection = FVector2(-2.0f, 0.0f); // 정규화된다
	E_EXPECT_TRUE(Step(Movement, State, Move, true).bDashStarted);
	E_EXPECT_NEAR(State.Velocity.X, -Movement.DashSpeed, 1.0e-3f);
	E_EXPECT_TRUE(State.IsDashing());
	// 대시 중 중력 무시 (공중 대시)
	FCharacterState2D Air;
	FCharacterMove2D  Up = MakeMove();
	Up.bDash             = true;
	Up.DashDirection     = FVector2(1.0f, 0.0f);
	E_EXPECT_TRUE(Step(Movement, Air, Up, false).bDashStarted);
	E_EXPECT_NEAR(Air.Velocity.Y, 0.0f, 0.0f);
	E_EXPECT_EQ(static_cast<int32>(Air.AirDashesUsed), 1);
	// 대시가 끝나면 보통 속력
	int32 Frames = 1;
	while (Air.IsDashing() && Frames < 60)
	{
		Step(Movement, Air, MakeMove(), false);
		++Frames;
	}
	E_EXPECT_EQ(Frames, static_cast<int32>(std::ceil(Movement.DashTime / Frame)));
	E_EXPECT_NEAR(Air.Velocity.X, Movement.MaxSpeed + 0.0f, Movement.AirDeceleration * Frame + 1.0e-3f);
	// 공중 대시는 MaxAirDashes(1)번 — 쿨다운이 지나도 착지 전에는 없음
	for (int32 Index = 0; Index < 30; ++Index)
	{
		Step(Movement, Air, MakeMove(), false);
	}
	E_EXPECT_FALSE(Step(Movement, Air, Up, false).bDashStarted);
	Step(Movement, Air, MakeMove(), true); // 착지 → 다시 채움
	E_EXPECT_TRUE(Step(Movement, Air, Up, true).bDashStarted);
	// 쿨다운 안에는 다시 대시하지 않는다
	FCharacterState2D Ground = Grounded();
	Step(Movement, Ground, Up, true);
	for (int32 Index = 0; Index < 12; ++Index) // 0.2초 > 대시 0.15초, < 쿨다운 0.35초
	{
		Step(Movement, Ground, MakeMove(), true);
	}
	E_EXPECT_FALSE(Step(Movement, Ground, Up, true).bDashStarted);
	// 방향이 0이면 입력 → 속도 X 부호
	FCharacterState2D Facing = Grounded();
	Facing.Velocity.X        = -10.0f;
	FCharacterMove2D NoDir   = MakeMove();
	NoDir.bDash              = true;
	Step(Movement, Facing, NoDir, true);
	E_EXPECT_TRUE(Facing.Velocity.X < 0.0f);
}

E_TEST(CharacterMovement2D_TopDownDiagonalNormalized)
{
	FCharacterMovement2DComponent Movement;
	Movement.Mode           = ECharacterMovement2DMode::TopDown;
	FCharacterState2D State;
	FCharacterMove2D  Move  = MakeMove();
	Move.Input              = FVector2(1.0f, 1.0f);
	Move.bJumpPressed       = true; // 탑다운은 점프 없음
	for (int32 Index = 0; Index < 60; ++Index)
	{
		E_EXPECT_FALSE(Step(Movement, State, Move, false).bJumped);
	}
	E_EXPECT_NEAR(State.Velocity.Length(), Movement.MaxSpeed, 1.0e-2f); // 대각선도 최대 속력
	E_EXPECT_NEAR(State.Velocity.X, State.Velocity.Y, 1.0e-3f);
	E_EXPECT_TRUE(State.bGrounded); // 중력 없음, 항상 바닥
	const FVector2 Clamped = CharacterMovement2DMath::ClampInput(FVector2(3.0f, 4.0f), ECharacterMovement2DMode::TopDown);
	E_EXPECT_NEAR(Clamped.X, 0.6f, 1.0e-5f);
	E_EXPECT_NEAR(Clamped.Y, 0.8f, 1.0e-5f);
	// 입력을 놓으면 감속
	State.Velocity = FVector2(600.0f, 0.0f);
	Step(Movement, State, MakeMove(), false);
	E_EXPECT_NEAR(State.Velocity.X, 600.0f - Movement.GroundDeceleration * Frame, 1.0e-2f);
}

// ---------------------------------------------------------------- 실제 2D 월드

namespace
{
	struct FWorld
	{
		FScene                     Scene;
		FPhysics2DSystem           Physics;
		FCharacterMovement2DSystem Characters;
		FEntity                    Pawn;

		FEntity AddBox(const char* Name, const FVector3& Center, const FVector2& Size, bool bOneWay = false)
		{
			const FEntity Entity = Scene.CreateEntity(Name);
			Scene.GetTransform(Entity).Position = Center;
			FBoxCollider2DComponent& Box        = Scene.GetRegistry().Emplace<FBoxCollider2DComponent>(Entity);
			Box.Size                            = Size;
			Box.bOneWay                         = bOneWay;
			return Entity;
		}
		FEntity AddPolygon(const char* Name, const std::string& Points)
		{
			const FEntity Entity = Scene.CreateEntity(Name);
			Scene.GetRegistry().Emplace<FPolygonCollider2DComponent>(Entity).Points = Points;
			return Entity;
		}
		void SpawnPawn(const FVector3& Position, const FCharacterMovement2DComponent& Movement = {})
		{
			Pawn                              = Scene.CreateEntity("Pawn");
			Scene.GetTransform(Pawn).Position = Position;
			Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Pawn) = Movement;
		}
		void Begin()
		{
			Scene.UpdateTransforms();
			Physics.Begin();
			Characters.Begin(Physics);
			Physics.SyncBodies(Scene);
			Characters.Sync(Scene);
		}
		void Tick(const FCharacterMove2D& Move)
		{
			Characters.Sync(Scene);
			Characters.SimulateCharacter(Scene, Pawn, Move);
			Characters.UpdateProxies();
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
		void Run(int32 Frames, float InputX = 0.0f)
		{
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				Tick(MakeMove(InputX));
			}
		}
		FVector3 Position() const { return Scene.GetTransform(Pawn).Position; }
		~FWorld()
		{
			Characters.End();
			Physics.End();
		}
	};
	// 기본 캡슐: 반지름 30, 높이 120 → 바닥(Z = 0) 위 중심 Z = 60
	constexpr float StandZ = 60.0f;
} // namespace

E_TEST(Character2DWorld_RunOnFlatGround)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	World.SpawnPawn(FVector3(0.0f, 7.0f, 100.0f));
	World.Begin();
	World.Run(60); // 내려앉기
	E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
	E_EXPECT_NEAR(World.Position().Y, 7.0f, 0.0f); // 깊이 유지
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
	const float StartX = World.Position().X;
	World.Run(60, 1.0f);
	// 0.1초 가속(600 / 6000) 동안 30cm 손해 → 1초에 약 570cm
	E_EXPECT_NEAR(World.Position().X - StartX, 600.0f - 0.5f * 600.0f * 0.1f, 15.0f);
	E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
	E_EXPECT_NEAR(World.Characters.GetVelocity(World.Pawn).X, 600.0f, 1.0f);
}

E_TEST(Character2DWorld_SlopeWalkableAndTooSteep)
{
	// 30도 오르막 (X 0 → 400, 높이 231)과 65도 벽 같은 경사 (X 1200 → 1300, 높이 214)
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	World.AddPolygon("Ramp", "0,0; 400,0; 400,230.94");
	World.AddBox("Plateau", FVector3(1700.0f, 0.0f, 115.47f), FVector2(2600.0f, 230.94f));
	World.SpawnPawn(FVector3(-200.0f, 0.0f, 100.0f));
	World.Begin();
	World.Run(30);
	World.Run(120, 1.0f); // 오르막을 올라 고원으로
	E_EXPECT_TRUE(World.Position().X > 450.0f);
	E_EXPECT_NEAR(World.Position().Z, 230.94f + StandZ, 3.0f);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));

	FWorld Steep;
	Steep.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	Steep.AddPolygon("Steep", "0,0; 100,0; 100,214.45"); // 65도
	Steep.SpawnPawn(FVector3(-200.0f, 0.0f, 100.0f));
	Steep.Begin();
	Steep.Run(30);
	Steep.Run(120, 1.0f);
	E_EXPECT_TRUE(Steep.Position().X < 60.0f);         // 경사를 넘지 못한다
	E_EXPECT_TRUE(Steep.Position().Z < StandZ + 60.0f); // 많이 오르지 못한다
	Steep.Run(60, 0.0f);
	E_EXPECT_TRUE(Steep.Characters.IsGrounded(Steep.Pawn)); // 미끄러져 내려와 평지에 선다
	E_EXPECT_NEAR(Steep.Position().Z, StandZ, 3.0f);
}

E_TEST(Character2DWorld_OneWayJumpThroughAndDropDown)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(4000.0f, 100.0f));
	World.AddBox("Platform", FVector3(0.0f, 0.0f, 140.0f), FVector2(400.0f, 20.0f), true); // 윗면 Z = 150
	World.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	World.Begin();
	World.Run(30);
	E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f); // 아래에서는 바닥에 서 있다
	World.Tick(MakeMove(0.0f, true));                 // 점프 (높이 약 205cm) → 발판을 뚫고 올라가 내려앉는다
	float MaxZ = 0.0f;
	for (int32 Index = 0; Index < 90; ++Index)
	{
		World.Tick(MakeMove(0.0f, false, true));
		MaxZ = std::max(MaxZ, World.Position().Z);
	}
	E_EXPECT_TRUE(MaxZ > 150.0f + StandZ + 20.0f); // 발이 발판 위로 넘어갔다
	E_EXPECT_NEAR(World.Position().Z, 150.0f + StandZ, 2.0f);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
	// 내려가기 → 바닥으로
	FCharacterMove2D Drop = MakeMove();
	Drop.bDropDown        = true;
	World.Tick(Drop);
	World.Run(60);
	E_EXPECT_NEAR(World.Position().Z, StandZ, 2.0f);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
}

E_TEST(Character2DWorld_CeilingStopsJump)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(4000.0f, 100.0f));
	World.AddBox("Ceiling", FVector3(0.0f, 0.0f, 210.0f), FVector2(400.0f, 20.0f)); // 아랫면 Z = 200
	World.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	World.Begin();
	World.Run(30);
	World.Tick(MakeMove(0.0f, true));
	float MaxZ  = 0.0f;
	bool  bFell = false;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		World.Tick(MakeMove(0.0f, false, true));
		MaxZ  = std::max(MaxZ, World.Position().Z);
		bFell = bFell || World.Characters.GetVelocity(World.Pawn).Y <= 0.0f;
	}
	E_EXPECT_TRUE(MaxZ <= 200.0f - StandZ + 1.0f); // 머리(중심 + 60)가 천장 아래
	E_EXPECT_TRUE(MaxZ > 200.0f - StandZ - 5.0f);  // 천장까지는 올라갔다
	E_EXPECT_TRUE(bFell);                          // 박치기 뒤 상승 속도가 없어졌다
	World.Run(60);
	E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
}

// 천장 모서리 (자동 조종이 단 밑면 모서리에 머리를 박은 채 멈췄던 보고 조사): 머리 바로 위 천장 아래·모서리 옆을 옆으로 움직일 때 막히지 않아야 한다
E_TEST(Character2DWorld_CeilingCornerDoesNotBlockSideways)
{
	// 1) 서서 걷기: 천장 아랫면이 머리(Z 120) 바로 위 — 0.5cm·3cm 틈. 천장 밑으로 들어가고 다시 나온다
	for (const float Gap : { 0.5f, 3.0f })
	{
		FWorld World;
		World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(4000.0f, 100.0f));
		World.AddBox("Ceiling", FVector3(200.0f, 0.0f, 120.0f + Gap + 50.0f), FVector2(400.0f, 100.0f)); // X 0~400, 아랫면 120 + Gap
		World.SpawnPawn(FVector3(-150.0f, 0.0f, StandZ + 0.5f));
		World.Begin();
		World.Run(30);
		E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
		World.Run(60, 1.0f); // 1초 오른쪽 → 천장 밑 (약 570cm 갈 수 있음)
		E_EXPECT_TRUE(World.Position().X > 300.0f);
		E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
		const float UnderX = World.Position().X;
		World.Run(60, -1.0f); // 되돌아 나온다
		E_LOG(LogPhysics, Display, "[천장 모서리 조사] 틈 {} 밑 X {:.2f} → 나옴 X {:.2f} Z {:.2f}", Gap, UnderX, World.Position().X, World.Position().Z);
		E_EXPECT_TRUE(World.Position().X < UnderX - 500.0f);
		E_EXPECT_TRUE(World.Position().X < -50.0f);
		E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
	}

	// 2) 점프해 천장 아랫면에 머리를 붙인 채 모서리 밖으로 옆 이동: 공중에서도 수평으로 계속 나아간다
	{
		FWorld World;
		World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(4000.0f, 100.0f));
		World.AddBox("Ceiling", FVector3(200.0f, 0.0f, 250.0f), FVector2(400.0f, 100.0f)); // 아랫면 Z = 200
		World.SpawnPawn(FVector3(60.0f, 0.0f, StandZ + 0.5f));
		World.Begin();
		World.Run(30);
		World.Tick(MakeMove(-1.0f, true));
		float StartX = World.Position().X;
		int32 Air    = 0;
		for (int32 Index = 0; Index < 120 && !World.Characters.IsGrounded(World.Pawn); ++Index, ++Air)
		{
			const float Before = World.Position().X;
			World.Tick(MakeMove(-1.0f, false, true));
			E_EXPECT_TRUE(World.Position().X < Before + 0.01f); // 뒤로 밀리거나 멈추지 않는다
		}
		E_EXPECT_TRUE(Air > 5);
		E_EXPECT_TRUE(World.Position().X < StartX - 0.5f * 600.0f * Frame * static_cast<float>(Air)); // 공중 수평 속도의 절반 이상
		World.Run(30, -1.0f);
		E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
		StartX = World.Position().X;
		World.Run(30, -1.0f);
		E_EXPECT_TRUE(World.Position().X < StartX - 200.0f);
	}

	// 3) 모서리 바로 옆에서 점프 (둥근 머리가 모서리에 닿음): 입력 없음/바깥/안쪽 모두 매달리지 않고 내려온다
	for (const float InputX : { 0.0f, -1.0f, 1.0f })
	{
		FWorld World;
		World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(4000.0f, 100.0f));
		World.AddBox("Ceiling", FVector3(200.0f, 0.0f, 250.0f), FVector2(400.0f, 100.0f)); // X 0~400, 아랫면 200
		World.SpawnPawn(FVector3(-20.0f, 0.0f, StandZ + 0.5f));                         // 캡슐 반지름 30 → 머리가 모서리 밑 10cm
		World.Begin();
		World.Run(30);
		World.Tick(MakeMove(0.0f, true));
		float MaxZ          = 0.0f;
		int32 GroundedFrame = -1;
		float MaxPenetration = 0.0f; // 캡슐이 천장(X 0~400, 아랫면 200) 안으로 들어간 깊이
		for (int32 Index = 0; Index < 90; ++Index)
		{
			World.Tick(MakeMove(InputX, false, true));
			const FVector3 P = World.Position();
			MaxZ             = std::max(MaxZ, P.Z);
			if (P.X >= 0.0f && P.X <= 400.0f)
			{
				MaxPenetration = std::max(MaxPenetration, P.Z + StandZ - 200.0f); // 머리 꼭대기
			}
			else if (P.X > -30.0f && P.X < 0.0f)
			{
				MaxPenetration = std::max(MaxPenetration, P.Z + 30.0f + std::sqrt(30.0f * 30.0f - P.X * P.X) - 200.0f); // 둥근 머리 ↔ 모서리
			}
			if (GroundedFrame < 0 && Index > 3 && World.Characters.IsGrounded(World.Pawn))
			{
				GroundedFrame = Index;
			}
		}
		// 입력 없음/바깥: 둥근 머리가 모서리에 밀려 바깥으로 빠져나가 계속 오른다 (모서리 보정 — 막히거나 매달리지 않음), 안쪽: 천장 밑에서 머리가 멈추고 옆으로 계속 간다
		E_LOG(LogPhysics, Display, "[천장 모서리 조사] 입력 {} 최고 Z {:.2f} 파고듦 {:.2f} 착지 프레임 {} 끝 X {:.2f}", InputX, MaxZ, MaxPenetration, GroundedFrame,
		      World.Position().X);
		E_EXPECT_TRUE(MaxPenetration <= 1.0f); // 천장을 뚫지 않는다
		E_EXPECT_TRUE(InputX > 0.0f ? World.Position().X > 400.0f : World.Position().X < -30.0f); // 안쪽은 천장 밑을 지나 반대편으로
		E_EXPECT_TRUE(GroundedFrame >= 0 && GroundedFrame < 60); // 1초 안에 착지 (매달리지 않음)
		E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
	}
}

E_TEST(Character2DWorld_RidesMovingPlatformAndPushesDynamicBox)
{
	FWorld        World;
	const FEntity Lift = World.AddBox("Lift", FVector3(0.0f, 0.0f, -10.0f), FVector2(400.0f, 20.0f)); // 윗면 Z = 0
	World.Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Lift).BodyType = EBodyType2D::Kinematic;
	World.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	World.Begin();
	for (int32 Index = 0; Index < 30; ++Index)
	{
		World.Tick(MakeMove());
	}
	E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
	const float StartX = World.Position().X;
	for (int32 Index = 0; Index < 60; ++Index) // 발판이 오른쪽 120cm/s, 위로 60cm/s
	{
		World.Scene.GetTransform(Lift).Position.X += 2.0f;
		World.Scene.GetTransform(Lift).Position.Z += 1.0f;
		World.Tick(MakeMove());
	}
	E_EXPECT_NEAR(World.Position().X - StartX, 120.0f, 6.0f); // 발판을 따라간다
	E_EXPECT_NEAR(World.Position().Z, 60.0f + StandZ, 3.0f);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));

	// 동적 상자를 밀면 상자가 밀려난다 (대리 키네마틱 바디)
	FWorld Push;
	Push.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	const FEntity Crate = Push.AddBox("Crate", FVector3(150.0f, 0.0f, 40.0f), FVector2(80.0f, 80.0f));
	Push.Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Crate).Mass = 10.0f;
	Push.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	Push.Begin();
	Push.Run(30);
	const float CrateStart = Push.Scene.GetTransform(Crate).Position.X;
	Push.Run(90, 1.0f);
	E_EXPECT_TRUE(Push.Scene.GetTransform(Crate).Position.X - CrateStart > 50.0f);
	E_EXPECT_TRUE(Push.Position().X > 100.0f);
}

// 동적 바디 겹침 깊이 (재조정 겹침 거부 — GameWorldCharacter2D.cpp): 위에 선 것은 0 근처, 옆/속으로 묻히면 그 깊이. 정적 바디는 세지 않는다
E_TEST(Character2DWorld_DynamicPenetration)
{
	FWorld        World;
	const FEntity Ground = World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	const FEntity Crate  = World.AddBox("Crate", FVector3(300.0f, 0.0f, 40.0f), FVector2(80.0f, 80.0f)); // 윗면 Z 80, 왼쪽 면 X 260
	World.Scene.GetRegistry().Emplace<FRigidBody2DComponent>(Crate).Mass = 10.0f;
	World.SpawnPawn(FVector3(0.0f, 0.0f, StandZ));
	World.Begin();
	(void)Ground;
	FCharacterState2D State = World.Characters.GetState(World.Pawn);
	E_EXPECT_NEAR(World.Characters.GetDynamicPenetration(World.Pawn), 0.0f, 0.5f); // 바닥(정적)에 선 채 — 정적은 세지 않는다
	State.Position = FVector2(300.0f, 80.0f + StandZ);
	World.Characters.SetState(World.Scene, World.Pawn, State);
	E_EXPECT_NEAR(World.Characters.GetDynamicPenetration(World.Pawn), 0.0f, 0.5f); // 상자 위에 선 자리
	State.Position = FVector2(260.0f - 30.0f + 10.0f, StandZ);
	World.Characters.SetState(World.Scene, World.Pawn, State);
	E_EXPECT_NEAR(World.Characters.GetDynamicPenetration(World.Pawn), 10.0f, 1.0f); // 옆으로 10cm 묻힘
	State.Position = FVector2(300.0f, 80.0f + StandZ - 15.0f);
	World.Characters.SetState(World.Scene, World.Pawn, State);
	E_EXPECT_NEAR(World.Characters.GetDynamicPenetration(World.Pawn), 15.0f, 1.0f); // 위에서 15cm 묻힘
}

// 같은 상태 + 같은 무브 → 같은 결과 (멀티플레이 재조정의 기준)
E_TEST(Character2DWorld_SameMovesSameResult)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	World.AddPolygon("Ramp", "300,0; 700,0; 700,230.94");
	World.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	World.Begin();
	World.Run(30);
	std::vector<FCharacterMove2D> Moves;
	for (int32 Index = 0; Index < 90; ++Index)
	{
		FCharacterMove2D Move = MakeMove(1.0f, Index == 10 || Index == 30, Index < 40);
		Move.bDash            = Index == 50;
		Moves.push_back(Move);
	}
	const FCharacterState2D Start = World.Characters.GetState(World.Pawn);
	for (const FCharacterMove2D& Move : Moves)
	{
		World.Characters.SimulateCharacter(World.Scene, World.Pawn, Move);
	}
	const FCharacterState2D First = World.Characters.GetState(World.Pawn);
	World.Characters.SetState(World.Scene, World.Pawn, Start);
	for (const FCharacterMove2D& Move : Moves)
	{
		World.Characters.SimulateCharacter(World.Scene, World.Pawn, Move);
	}
	const FCharacterState2D Second = World.Characters.GetState(World.Pawn);
	E_EXPECT_TRUE(First.Position == Second.Position && First.Velocity == Second.Velocity);
	E_EXPECT_TRUE(First.bGrounded == Second.bGrounded && First.JumpsUsed == Second.JumpsUsed);
	E_EXPECT_TRUE(First.Position.X > Start.Position.X + 300.0f);
}

// 대리 바디: 트리거·레이캐스트가 캐릭터를 본다 (이동기 자신은 트리거에 막히지 않는다)
E_TEST(Character2DWorld_ProxySeenByTriggerAndRaycast)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	const FEntity Coin = World.AddBox("Coin", FVector3(300.0f, 0.0f, 60.0f), FVector2(40.0f, 40.0f));
	World.Scene.GetRegistry().Get<FBoxCollider2DComponent>(Coin).bIsTrigger = true;
	World.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	World.Begin();
	World.Run(20);
	FPhysics2DHit Hit;
	E_EXPECT_TRUE(World.Physics.Raycast(FVector2(-300.0f, 60.0f), FVector2(1.0f, 0.0f), 1000.0f, Hit));
	E_EXPECT_TRUE(Hit.Entity == World.Pawn);
	E_EXPECT_NEAR(Hit.Position.X, World.Position().X - 30.0f, 1.0f);
	bool bEntered = false;
	for (int32 Index = 0; Index < 60; ++Index)
	{
		World.Tick(MakeMove(1.0f));
		for (const FCollisionEvent& Event : World.Physics.GetCollisionEvents())
		{
			bEntered = bEntered || (Event.Type == ECollisionEventType::TriggerEnter && Event.Self == Coin && Event.Other == World.Pawn);
		}
	}
	E_EXPECT_TRUE(bEntered);
	E_EXPECT_TRUE(World.Position().X > 400.0f); // 트리거에 막히지 않는다
}

// 타일식 45도 계단 (셀마다 삼각형 + 아래 상자): 꼭짓점 고스트 접촉(내부 모서리)에 걸리지 않고 올라간다
E_TEST(Character2DWorld_TileSlopeSeamsDoNotSnag)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const float X = 50.0f * static_cast<float>(Index);
		const float Z = 50.0f * static_cast<float>(Index);
		World.AddPolygon("Tri", std::format("{},{}; {},{}; {},{}", X, Z, X + 50.0f, Z, X + 50.0f, Z + 50.0f));
		if (Index > 0)
		{
			World.AddBox("Fill", FVector3(X + 25.0f, 0.0f, Z * 0.5f), FVector2(50.0f, Z)); // 삼각형 아래 채움 (셀 상자 열)
		}
	}
	World.AddBox("Plateau", FVector3(1200.0f, 0.0f, 100.0f), FVector2(2000.0f, 200.0f)); // 윗면 Z 200, X 200 ~ 2200
	World.SpawnPawn(FVector3(-300.0f, 0.0f, 80.0f));
	World.Begin();
	World.Run(20);
	World.Run(110, 1.0f);
	E_EXPECT_TRUE(World.Position().X > 500.0f);
	E_EXPECT_NEAR(World.Position().Z, 200.0f + StandZ, 2.0f);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
	// 내려오기도 (오른쪽 → 왼쪽, 바닥에 붙어 내려온다)
	World.Run(110, -1.0f);
	E_EXPECT_TRUE(World.Position().X < -50.0f);
	E_EXPECT_NEAR(World.Position().Z, StandZ, 2.0f);
}

// ---------------------------------------------------------------- 캐릭터끼리 (CharacterCollision)

namespace
{
	using ECollision = FCharacterMovement2DComponent::ECharacterCollision;

	// 두 캐릭터: Pawn(A)은 입력, Other(B)는 입력 없이 (서 있거나 떨어진다)
	struct FDuo : FWorld
	{
		FEntity Other;
		bool    bOtherFirst = false; // 무브 순서 (게임 월드는 엔티티 순서)

		void Setup(ECollision ModeA, ECollision ModeB, const FVector3& PositionA, const FVector3& PositionB)
		{
			AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
			FCharacterMovement2DComponent A;
			A.CharacterCollision = ModeA;
			SpawnPawn(PositionA, A);
			Other                              = Scene.CreateEntity("Other");
			Scene.GetTransform(Other).Position = PositionB;
			Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Other).CharacterCollision = ModeB;
			Begin();
		}
		void Step(float InputX)
		{
			Characters.Sync(Scene);
			if (bOtherFirst)
			{
				Characters.SimulateCharacter(Scene, Other, MakeMove());
			}
			Characters.SimulateCharacter(Scene, Pawn, MakeMove(InputX));
			if (!bOtherFirst)
			{
				Characters.SimulateCharacter(Scene, Other, MakeMove());
			}
			Characters.UpdateProxies();
			Physics.Update(Scene, Frame);
			Scene.UpdateTransforms();
		}
		void Run(int32 Frames, float InputX)
		{
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				Step(InputX);
			}
		}
		FVector3 OtherPosition() const { return Scene.GetTransform(Other).Position; }
	};
} // namespace

// 막기/밀기/통과: 기본 Ignore는 통과, 둘 다 Ignore가 아니면 막히고, Push는 상대를 민다 (벽 안으로는 못 민다)
E_TEST(Character2DWorld_CharacterBlockPushAndIgnore)
{
	// 기본(Ignore): 통과
	{
		FDuo World;
		World.Setup(ECollision::Ignore, ECollision::Ignore, FVector3(0.0f, 0.0f, 80.0f), FVector3(200.0f, 0.0f, 80.0f));
		World.Run(20, 0.0f);
		World.Run(60, 1.0f);
		E_EXPECT_TRUE(World.Position().X > 300.0f);
		E_EXPECT_NEAR(World.OtherPosition().X, 200.0f, 0.5f);
	}
	// 한쪽만 Block: 상대가 Ignore면 여전히 통과
	{
		FDuo World;
		World.Setup(ECollision::Block, ECollision::Ignore, FVector3(0.0f, 0.0f, 80.0f), FVector3(200.0f, 0.0f, 80.0f));
		World.Run(20, 0.0f);
		World.Run(60, 1.0f);
		E_EXPECT_TRUE(World.Position().X > 300.0f);
	}
	// 둘 다 Block: 캡슐(반지름 30)끼리 닿은 자리에서 멈춘다, 상대는 그대로
	{
		FDuo World;
		World.Setup(ECollision::Block, ECollision::Block, FVector3(0.0f, 0.0f, 80.0f), FVector3(200.0f, 0.0f, 80.0f));
		World.Run(20, 0.0f);
		World.Run(60, 1.0f);
		E_EXPECT_NEAR(World.Position().X, 200.0f - 60.0f, 2.0f);
		E_EXPECT_NEAR(World.OtherPosition().X, 200.0f, 0.5f);
		E_EXPECT_NEAR(World.Position().Z, StandZ, 1.5f);
	}
	// Push: 상대를 밀며 나아간다 (상대는 바닥에 선 채)
	{
		FDuo World;
		World.Setup(ECollision::Push, ECollision::Block, FVector3(0.0f, 0.0f, 80.0f), FVector3(200.0f, 0.0f, 80.0f));
		World.Run(20, 0.0f);
		World.Run(60, 1.0f);
		E_EXPECT_TRUE(World.OtherPosition().X > 300.0f);
		E_EXPECT_NEAR(World.OtherPosition().X - World.Position().X, 60.0f, 3.0f);
		E_EXPECT_NEAR(World.OtherPosition().Z, StandZ, 1.5f);
	}
	// Push + 벽: 상대는 벽에서 멈추고 나도 멈춘다
	{
		FDuo World;
		World.AddBox("Wall", FVector3(350.0f, 0.0f, 100.0f), FVector2(100.0f, 400.0f)); // 왼쪽 면 X = 300
		World.Setup(ECollision::Push, ECollision::Block, FVector3(0.0f, 0.0f, 80.0f), FVector3(200.0f, 0.0f, 80.0f));
		World.Run(20, 0.0f);
		World.Run(90, 1.0f);
		E_EXPECT_NEAR(World.OtherPosition().X, 300.0f - 30.0f, 2.0f);
		E_EXPECT_NEAR(World.Position().X, 300.0f - 90.0f, 3.0f);
	}
}

// 밟기: 위에서 떨어져 상대 위에 선다 + Stomped 이벤트 (상대 엔티티)
E_TEST(Character2DWorld_StompLandsOnCharacter)
{
	for (const bool bOtherFirst : { false, true })
	{
		FDuo World;
		World.bOtherFirst = bOtherFirst;
		World.Setup(ECollision::Block, ECollision::Block, FVector3(10.0f, 0.0f, 500.0f), FVector3(0.0f, 0.0f, 61.0f));
		std::vector<FCharacterMovement2DSystem::FEvent> Events;
		bool                                            bStomped = false;
		for (int32 Index = 0; Index < 90; ++Index)
		{
			World.Step(0.0f);
			World.Characters.ConsumeEvents(Events);
			for (const FCharacterMovement2DSystem::FEvent& Event : Events)
			{
				if (Event.Entity == World.Pawn && Event.Events.bStomped)
				{
					bStomped = true;
					E_EXPECT_TRUE(Event.Events.bLanded);
					E_EXPECT_EQ(Event.Events.StompedEntity, World.Other.ToId());
				}
				E_EXPECT_FALSE(Event.Entity == World.Other && Event.Events.bStomped); // 바닥에 착지한 쪽은 밟기 아님
			}
			Events.clear();
		}
		E_EXPECT_TRUE(bStomped);
		// 상대 캡슐 머리 위 (가로 10cm 어긋남: 반원 중심 거리 60 → 높이 59.16), 둥근 머리에서 미끄러지지 않고 상대도 밀리지 않는다
		E_EXPECT_NEAR(World.Position().Z, StandZ + 60.0f + 59.16f, 1.5f);
		E_EXPECT_NEAR(World.Position().X, 10.0f, 0.5f);
		E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
		E_EXPECT_NEAR(World.OtherPosition().Z, StandZ, 1.5f);
		E_EXPECT_NEAR(World.OtherPosition().X, 0.0f, 0.5f);
	}
}

namespace
{
	// 줄지어 선 캐릭터: Pawn(첫째)만 입력, 나머지는 입력 없이. 무브 순서 = 만든 순서
	struct FLine : FWorld
	{
		std::vector<FEntity> Others;

		void Setup(const FCharacterMovement2DComponent& PawnMovement, const std::vector<std::pair<ECollision, float>>& OthersSetup)
		{
			AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
			SpawnPawn(FVector3(0.0f, 0.0f, 80.0f), PawnMovement);
			for (const auto& [Mode, X] : OthersSetup)
			{
				const FEntity Entity                = Scene.CreateEntity("Other");
				Scene.GetTransform(Entity).Position = FVector3(X, 0.0f, 80.0f);
				Scene.GetRegistry().Emplace<FCharacterMovement2DComponent>(Entity).CharacterCollision = Mode;
				Others.push_back(Entity);
			}
			Begin();
		}
		void Run(int32 Frames, float InputX)
		{
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				Characters.Sync(Scene);
				Characters.SimulateCharacter(Scene, Pawn, MakeMove(InputX));
				for (const FEntity Entity : Others)
				{
					Characters.SimulateCharacter(Scene, Entity, MakeMove());
				}
				Characters.UpdateProxies();
				Physics.Update(Scene, Frame);
				Scene.UpdateTransforms();
			}
		}
		float X(size_t Index) const { return Scene.GetTransform(Others[Index]).Position.X; }
	};

	FCharacterMovement2DComponent PushMovement(float Strength = 1.0f)
	{
		FCharacterMovement2DComponent Movement;
		Movement.CharacterCollision = ECollision::Push;
		Movement.PushStrength       = Strength;
		return Movement;
	}
} // namespace

// 밀기 연쇄: 밀린 캐릭터(Block/Push 상관없이)가 앞 캐릭터를 민다 — 줄 전체가 붙어서 나아가고, 벽에 닿으면 모두 멈춘다. Ignore는 사슬에 끼지 않는다
E_TEST(Character2DWorld_PushChainsThroughCharacters)
{
	{
		FLine World;
		World.Setup(PushMovement(), { { ECollision::Block, 150.0f }, { ECollision::Push, 300.0f }, { ECollision::Block, 450.0f } });
		World.Run(20, 0.0f);
		World.Run(90, 1.0f);
		E_EXPECT_TRUE(World.X(2) > 480.0f);
		E_EXPECT_NEAR(World.X(0) - World.Position().X, 60.0f, 3.0f);
		E_EXPECT_NEAR(World.X(1) - World.X(0), 60.0f, 3.0f);
		E_EXPECT_NEAR(World.X(2) - World.X(1), 60.0f, 3.0f);
		E_EXPECT_NEAR(World.Scene.GetTransform(World.Others[2]).Position.Z, StandZ, 1.5f);
	}
	// 벽: 맨 앞이 벽에서 멈추면 줄 전체가 멈춘다 (벽 안으로 밀려 들어가지 않는다)
	{
		FLine World;
		World.AddBox("Wall", FVector3(550.0f, 0.0f, 100.0f), FVector2(100.0f, 400.0f)); // 왼쪽 면 X = 500
		World.Setup(PushMovement(), { { ECollision::Block, 150.0f }, { ECollision::Block, 300.0f } });
		World.Run(20, 0.0f);
		World.Run(120, 1.0f);
		E_EXPECT_NEAR(World.X(1), 500.0f - 30.0f, 2.0f);
		E_EXPECT_NEAR(World.X(0), 500.0f - 90.0f, 3.0f);
		E_EXPECT_NEAR(World.Position().X, 500.0f - 150.0f, 4.0f);
	}
	// 깊이 상한: 밀리는 캐릭터는 최대 4 — 다섯째부터는 벽처럼 막는다
	{
		FLine World;
		World.Setup(PushMovement(), { { ECollision::Block, 100.0f }, { ECollision::Block, 200.0f }, { ECollision::Block, 300.0f },
		                              { ECollision::Block, 400.0f }, { ECollision::Block, 500.0f } });
		World.Run(20, 0.0f);
		World.Run(120, 1.0f);
		E_EXPECT_NEAR(World.X(4), 500.0f, 0.5f);
		E_EXPECT_NEAR(World.X(3), 500.0f - 60.0f, 3.0f);
	}
}

// 밀기 세기: 작을수록 느리게(무겁게) 민다, 0이면 Block과 같다 (기본 1 = 이전 동작)
E_TEST(Character2DWorld_PushStrength)
{
	float Distances[3] = {};
	const float Strengths[3] = { 1.0f, 0.5f, 0.0f };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FLine World;
		World.Setup(PushMovement(Strengths[Index]), { { ECollision::Block, 100.0f } });
		World.Run(20, 0.0f);
		World.Run(60, 1.0f); // 가속 0.1초 + 접촉까지 몇 프레임
		const float Before = World.X(0);
		World.Run(60, 1.0f);
		Distances[Index] = World.X(0) - Before;
		E_EXPECT_TRUE(World.X(0) - World.Position().X > 57.0f); // 겹치지 않는다
	}
	E_EXPECT_TRUE(Distances[0] > 200.0f);
	E_EXPECT_TRUE(Distances[1] > 30.0f && Distances[1] < Distances[0] * 0.6f);
	E_EXPECT_NEAR(Distances[2], 0.0f, 0.5f);
}

// 밀림 저항: 넘겨받은 거리 ÷ (1 + r) — 무거운 캐릭터는 덜 밀린다 (r = 0이면 이전 동작과 같은 거리), 사슬은 단계마다 받는 쪽 저항으로
// 나눈다(뒤에 무거운 캐릭터가 서 있으면 줄 전체가 덜 나아간다). 같은 무브 반복은 같은 결과
E_TEST(Character2DWorld_PushResistance)
{
	const auto Measure = [](std::vector<float> Resistances, std::vector<float> Xs, std::vector<float>* OutFinal = nullptr, float Strength = 1.0f) {
		FLine World;
		std::vector<std::pair<ECollision, float>> Setup;
		for (const float X : Xs)
		{
			Setup.push_back({ ECollision::Block, X });
		}
		World.Setup(PushMovement(Strength), Setup);
		for (size_t Index = 0; Index < Resistances.size(); ++Index)
		{
			World.Scene.GetRegistry().Get<FCharacterMovement2DComponent>(World.Others[Index]).PushResistance = Resistances[Index];
		}
		World.Run(20, 0.0f);
		World.Run(60, 1.0f);
		const float Before = World.X(0);
		World.Run(60, 1.0f);
		E_EXPECT_TRUE(World.X(0) - World.Position().X > 57.0f); // 겹치지 않는다
		if (OutFinal != nullptr)
		{
			OutFinal->clear();
			OutFinal->push_back(World.Position().X);
			for (size_t Index = 0; Index < World.Others.size(); ++Index)
			{
				OutFinal->push_back(World.X(Index));
			}
		}
		return World.X(0) - Before;
	};
	// 한 캐릭터: 저항 r = 미는 쪽 세기 1 / (1 + r)와 같은 식 (같은 거리), 클수록 덜 밀린다. 미는 쪽 속도도 막혀 줄어 들므로
	// 거리는 비례보다 더 줄어든다 (세기와 같은 성질)
	const float Free  = Measure({ 0.0f }, { 100.0f });
	const float Half  = Measure({ 1.0f }, { 100.0f });
	const float Heavy = Measure({ 3.0f }, { 100.0f });
	E_EXPECT_TRUE(Free > 200.0f);
	E_EXPECT_TRUE(Half < Free * 0.6f && Half > 10.0f);
	E_EXPECT_TRUE(Heavy < Half * 0.6f && Heavy > 1.0f);
	E_EXPECT_NEAR(Half, Measure({ 0.0f }, { 100.0f }, nullptr, 0.5f), 0.01f);
	E_EXPECT_NEAR(Heavy, Measure({ 0.0f }, { 100.0f }, nullptr, 0.25f), 0.01f);
	// 사슬: 앞이 가볍고 뒤가 무거우면(0, 1) 둘 다 저항 0인 줄보다 덜 나아가고, (1, 1)이면 더 덜 나아간다
	const float ChainFree = Measure({ 0.0f, 0.0f }, { 100.0f, 200.0f });
	const float ChainBack = Measure({ 0.0f, 1.0f }, { 100.0f, 200.0f });
	const float ChainBoth = Measure({ 1.0f, 1.0f }, { 100.0f, 200.0f });
	E_LOG(LogPhysics, Display, "[밀림 저항] 하나 {:.1f} / {:.1f} / {:.1f}, 사슬 {:.1f} / {:.1f} / {:.1f}", Free, Half, Heavy, ChainFree, ChainBack, ChainBoth);
	E_EXPECT_TRUE(ChainFree > 150.0f);
	E_EXPECT_TRUE(ChainBack < ChainFree * 0.65f && ChainBack > 5.0f);
	E_EXPECT_TRUE(ChainBoth < ChainBack && ChainBoth > 0.5f);
	// 결정적: 같은 장면·같은 무브를 두 번 돌리면 비트 단위로 같다
	std::vector<float> First, Second;
	Measure({ 0.5f, 2.0f }, { 100.0f, 200.0f }, &First);
	Measure({ 0.5f, 2.0f }, { 100.0f, 200.0f }, &Second);
	E_EXPECT_TRUE(First == Second);
}

// 같은 상태 + 같은 무브 → 같은 결과 (다른 캐릭터가 막는 월드에서도)
E_TEST(Character2DWorld_SameMovesSameResultWithCharacters)
{
	FDuo World;
	World.Setup(ECollision::Block, ECollision::Block, FVector3(0.0f, 0.0f, 80.0f), FVector3(250.0f, 0.0f, 80.0f));
	World.Run(30, 0.0f);
	std::vector<FCharacterMove2D> Moves;
	for (int32 Index = 0; Index < 90; ++Index)
	{
		Moves.push_back(MakeMove(1.0f, Index == 20 || Index == 40, Index < 50));
	}
	const FCharacterState2D Start = World.Characters.GetState(World.Pawn);
	for (const FCharacterMove2D& Move : Moves)
	{
		World.Characters.SimulateCharacter(World.Scene, World.Pawn, Move);
	}
	const FCharacterState2D First = World.Characters.GetState(World.Pawn);
	World.Characters.SetState(World.Scene, World.Pawn, Start);
	for (const FCharacterMove2D& Move : Moves)
	{
		World.Characters.SimulateCharacter(World.Scene, World.Pawn, Move);
	}
	const FCharacterState2D Second = World.Characters.GetState(World.Pawn);
	E_EXPECT_TRUE(First.Position == Second.Position && First.Velocity == Second.Velocity);
	E_EXPECT_TRUE(First.bGrounded == Second.bGrounded && First.JumpsUsed == Second.JumpsUsed);
	E_EXPECT_TRUE(First.Position.X > 250.0f); // 점프로 상대를 넘거나 위에 올라섰다 (막히기만 하지 않음)
}

// ---------------------------------------------------------------- 넉백/발사 (LaunchCharacter/AddKnockback — CharacterMovement2D.h)

// 발사: 성분마다 더하기/덮어쓰기, 위로 향하면 바닥을 떠나고 바닥 점프를 쓴 것으로 (공중 점프는 남는다), 대시는 끝난다
E_TEST(CharacterMovement2D_LaunchAddsOrOverrides)
{
	FCharacterMovement2DComponent Movement;
	Movement.MaxJumps          = 2;
	FCharacterState2D State    = Grounded();
	State.Velocity             = FVector2(200.0f, 0.0f);
	FCharacterMove2D Launch    = MakeMove();
	Launch.bLaunch             = true;
	Launch.LaunchVelocity      = FVector2(100.0f, 800.0f);
	FCharacterMove2DEvents Events;
	CharacterMovement2DMath::BeginMove(Movement, State, Launch, Gravity, FVector2(), Events);
	// X는 더하기 (200 + 100) 뒤 공중 감속 한 프레임, Z는 800 + 중력 한 프레임
	E_EXPECT_NEAR(State.Velocity.X, 300.0f - Movement.AirDeceleration * Frame, 1.0e-3f);
	E_EXPECT_NEAR(State.Velocity.Y, 800.0f + Gravity * Movement.GravityScale * Frame, 1.0e-3f);
	E_EXPECT_FALSE(State.bGrounded);
	E_EXPECT_EQ(static_cast<int32>(State.JumpsUsed), 1);
	E_EXPECT_FALSE(Events.bJumped);
	// 공중 점프는 남아 있다
	FCharacterMove2DEvents AirJump;
	CharacterMovement2DMath::BeginMove(Movement, State, MakeMove(0.0f, true), Gravity, FVector2(), AirJump);
	E_EXPECT_TRUE(AirJump.bJumped && AirJump.JumpIndex == 2);

	// 덮어쓰기: 대시 중에도 대시를 끝내고 정확히 그 속도에서
	FCharacterState2D Dashing = Grounded();
	FCharacterMove2D  Dash    = MakeMove();
	Dash.bDash                = true;
	Dash.DashDirection        = FVector2(1.0f, 0.0f);
	Step(Movement, Dashing, Dash, true);
	E_EXPECT_TRUE(Dashing.IsDashing());
	FCharacterMove2D Override = MakeMove();
	Override.bLaunch          = true;
	Override.bLaunchOverrideX = true;
	Override.bLaunchOverrideY = true;
	Override.LaunchVelocity   = FVector2(-400.0f, 0.0f);
	Step(Movement, Dashing, Override, true);
	E_EXPECT_FALSE(Dashing.IsDashing());
	E_EXPECT_NEAR(Dashing.Velocity.X, -400.0f + Movement.GroundDeceleration * Frame, 1.0e-3f); // 수평 발사는 바닥에 남는다 → 지상 감속
	E_EXPECT_TRUE(Dashing.bGrounded);
	// 유한하지 않은 값은 0, 큰 값은 자른다
	FCharacterState2D Bad = Grounded();
	FCharacterMove2D  Nan = MakeMove();
	Nan.bLaunch           = true;
	Nan.bLaunchOverrideX  = true;
	Nan.LaunchVelocity    = FVector2(std::nanf(""), 1.0e9f);
	FCharacterMove2DEvents Ignored;
	CharacterMovement2DMath::BeginMove(Movement, Bad, Nan, Gravity, FVector2(), Ignored);
	E_EXPECT_TRUE(std::isfinite(Bad.Velocity.X) && std::isfinite(Bad.Velocity.Y));
	E_EXPECT_TRUE(Bad.Velocity.Y <= FCharacterMove2D::MaxLaunchSpeed);
}

// 넉백 경직: 경직 동안 입력·점프·대시를 무시하고 KnockbackDeceleration으로 줄어들며, 끝나면 다시 입력을 따른다
E_TEST(CharacterMovement2D_KnockbackStunIgnoresInput)
{
	FCharacterMovement2DComponent Movement;
	Movement.KnockbackDeceleration = 1200.0f;
	FCharacterState2D State        = Grounded();
	FCharacterMove2D  Hit          = MakeMove(1.0f, true); // 오른쪽 입력 + 점프를 눌러도
	Hit.bLaunch                    = true;
	Hit.bLaunchOverrideX           = true;
	Hit.LaunchVelocity             = FVector2(-600.0f, 0.0f);
	Hit.StunSeconds                = 0.25f;
	Hit.bDash                      = true;
	FCharacterMove2DEvents Events  = Step(Movement, State, Hit, true);
	E_EXPECT_FALSE(Events.bJumped || Events.bDashStarted);
	E_EXPECT_NEAR(State.Velocity.X, -600.0f + 1200.0f * Frame, 1.0e-3f);
	E_EXPECT_TRUE(State.IsStunned());
	int32 StunFrames = 1;
	while (State.IsStunned() && StunFrames < 60)
	{
		Events = Step(Movement, State, MakeMove(1.0f, true), true);
		E_EXPECT_FALSE(Events.bJumped);
		++StunFrames;
	}
	// 0.25초 = 15프레임 (경직이 남아 있던 무브까지 입력 무시)
	E_EXPECT_EQ(StunFrames, 15);
	E_EXPECT_NEAR(State.Velocity.X, -600.0f + 1200.0f * Frame * 15.0f, 1.0e-2f);
	// 경직이 끝나면 입력 (반대 방향 = max(가속, 감속))
	const float Before = State.Velocity.X;
	Step(Movement, State, MakeMove(1.0f), true);
	E_EXPECT_NEAR(State.Velocity.X, Before + std::max(Movement.GroundAcceleration, Movement.GroundDeceleration) * Frame, 1.0e-2f);
}

// 실제 월드: 바닥에서 위로 발사하면 바닥 붙이기 없이 뜨고, 넉백 궤적은 같은 무브 → 같은 결과
E_TEST(Character2DWorld_KnockbackTrajectoryDeterministic)
{
	FWorld World;
	World.AddBox("Ground", FVector3(0.0f, 0.0f, -50.0f), FVector2(10000.0f, 100.0f));
	World.SpawnPawn(FVector3(0.0f, 0.0f, 80.0f));
	World.Begin();
	World.Run(30);
	E_EXPECT_TRUE(World.Characters.IsGrounded(World.Pawn));
	const FCharacterState2D Start = World.Characters.GetState(World.Pawn);

	std::vector<FCharacterMove2D> Moves;
	for (int32 Index = 0; Index < 80; ++Index)
	{
		Moves.push_back(MakeMove(1.0f)); // 내내 오른쪽 입력
	}
	Moves[0].bLaunch          = true;
	Moves[0].bLaunchOverrideX = true;
	Moves[0].bLaunchOverrideY = true;
	Moves[0].LaunchVelocity   = FVector2(-700.0f, 600.0f);
	Moves[0].StunSeconds      = 0.3f;
	const auto RunMoves = [&]() {
		World.Characters.SetState(World.Scene, World.Pawn, Start);
		float PeakZ = Start.Position.Y, MinX = Start.Position.X;
		for (const FCharacterMove2D& Move : Moves)
		{
			World.Characters.SimulateCharacter(World.Scene, World.Pawn, Move);
			PeakZ = std::max(PeakZ, World.Characters.GetState(World.Pawn).Position.Y);
			MinX  = std::min(MinX, World.Characters.GetState(World.Pawn).Position.X);
		}
		return std::make_pair(PeakZ, MinX);
	};
	World.Characters.SetState(World.Scene, World.Pawn, Start);
	World.Characters.SimulateCharacter(World.Scene, World.Pawn, Moves[0]);
	E_EXPECT_FALSE(World.Characters.IsGrounded(World.Pawn)); // 바닥에 다시 붙지 않고 떴다
	E_EXPECT_TRUE(World.Characters.IsStunned(World.Pawn));
	const auto [PeakZ, MinX] = RunMoves();
	const FCharacterState2D First = World.Characters.GetState(World.Pawn);
	RunMoves();
	const FCharacterState2D Second = World.Characters.GetState(World.Pawn);
	E_EXPECT_TRUE(First.Position == Second.Position && First.Velocity == Second.Velocity && First.StunTimer == Second.StunTimer);
	// 위로 v²/2g ≈ 600² / (2 × 2942) ≈ 61cm, 경직 동안 입력과 반대로 밀려났다가 다시 입력 쪽으로
	E_EXPECT_NEAR(PeakZ - Start.Position.Y, 600.0f * 600.0f / (2.0f * 980.665f * 3.0f), 6.0f);
	E_EXPECT_TRUE(MinX < Start.Position.X - 100.0f);
	E_EXPECT_TRUE(First.bGrounded && First.Position.X > MinX + 50.0f);

	// 시스템 API: 한 프레임에 쌓인 발사는 합쳐져 무브에 실린다
	World.Characters.LaunchCharacter(World.Pawn, FVector3(100.0f, 0.0f, 0.0f), false, false);
	World.Characters.LaunchCharacter(World.Pawn, FVector3(50.0f, 0.0f, 300.0f), false, true);
	World.Characters.AddKnockback(World.Pawn, FVector3(0.0f, 0.0f, 0.0f), 0.5f); // X 덮어쓰기 0, Z는 0이라 덮어쓰지 않음
	FCharacterMove2D Merged = MakeMove();
	E_EXPECT_TRUE(World.Characters.MergePendingLaunch(World.Pawn, Merged));
	E_EXPECT_TRUE(Merged.bLaunch && Merged.bLaunchOverrideX && Merged.bLaunchOverrideY);
	E_EXPECT_NEAR(Merged.LaunchVelocity.X, 0.0f, 0.0f);
	E_EXPECT_NEAR(Merged.LaunchVelocity.Y, 300.0f, 0.0f);
	E_EXPECT_NEAR(Merged.StunSeconds, 0.5f, 0.0f);
	E_EXPECT_FALSE(World.Characters.HasPendingLaunch(World.Pawn));
}

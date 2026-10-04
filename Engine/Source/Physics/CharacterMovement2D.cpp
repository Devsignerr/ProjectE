#include "Physics/CharacterMovement2D.h"

#include <algorithm>
#include <cmath>

namespace CharacterMovement2DMath
{
	FVector2 ClampInput(const FVector2& Input, ECharacterMovement2DMode Mode)
	{
		if (!std::isfinite(Input.X) || !std::isfinite(Input.Y))
		{
			return FVector2();
		}
		FVector2 Result = Mode == ECharacterMovement2DMode::Platformer ? FVector2(Input.X, 0.0f) : Input;
		const float LengthSquared = Result.X * Result.X + Result.Y * Result.Y;
		if (LengthSquared > 1.0f)
		{
			Result = Result / std::sqrt(LengthSquared);
		}
		return Result;
	}

	float Approach(float Current, float Target, float MaxDelta)
	{
		if (Current < Target)
		{
			return std::min(Current + MaxDelta, Target);
		}
		return std::max(Current - MaxDelta, Target);
	}

	FVector2 ApproachVector(const FVector2& Current, const FVector2& Target, float MaxDelta)
	{
		const FVector2 Delta    = Target - Current;
		const float    Distance = Delta.Length();
		if (Distance <= MaxDelta || Distance <= 1.0e-6f)
		{
			return Target;
		}
		return Current + Delta * (MaxDelta / Distance);
	}

	float GetWalkableNormalY(const FCharacterMovement2DComponent& Movement)
	{
		return std::cos(std::clamp(Movement.MaxSlopeAngle, 0.0f, 89.0f) * FMath::DegToRad);
	}

	float ComputeJumpHeight(const FCharacterMovement2DComponent& Movement, float GravityZ)
	{
		const float Gravity = std::abs(GravityZ * Movement.GravityScale);
		return Gravity > 1.0e-6f ? Movement.JumpVelocity * Movement.JumpVelocity / (2.0f * Gravity) : 0.0f;
	}

	namespace
	{
		FVector2 Normalized(const FVector2& V)
		{
			const float Length = V.Length();
			return Length > 1.0e-6f ? V / Length : FVector2();
		}

		FVector2 ResolveDashDirection(const FCharacterMovement2DComponent& Movement, const FCharacterState2D& State, const FCharacterMove2D& Move)
		{
			FVector2 Direction = std::isfinite(Move.DashDirection.X) && std::isfinite(Move.DashDirection.Y) ? Normalized(Move.DashDirection) : FVector2();
			if (Direction.LengthSquared() <= 0.0f)
			{
				Direction = Normalized(ClampInput(Move.Input, Movement.Mode));
			}
			if (Direction.LengthSquared() <= 0.0f)
			{
				Direction = FVector2(State.Velocity.X < 0.0f ? -1.0f : 1.0f, 0.0f);
			}
			return Direction;
		}

		float ClampLaunchComponent(float Value)
		{
			return std::isfinite(Value) ? std::clamp(Value, -FCharacterMove2D::MaxLaunchSpeed, FCharacterMove2D::MaxLaunchSpeed) : 0.0f;
		}

		// 발사/넉백 (무브 처음 — 헤더 BeginMove 주석)
		void ApplyLaunch(FCharacterState2D& State, const FCharacterMove2D& Move, bool bTopDown)
		{
			const FVector2 Launch(ClampLaunchComponent(Move.LaunchVelocity.X), ClampLaunchComponent(Move.LaunchVelocity.Y));
			State.Velocity.X = Move.bLaunchOverrideX ? Launch.X : State.Velocity.X + Launch.X;
			State.Velocity.Y = Move.bLaunchOverrideY ? Launch.Y : State.Velocity.Y + Launch.Y;
			State.DashTimer         = 0.0f; // 대시 중이었으면 끝 (속도는 발사 결과)
			State.bJumpCutAvailable = false;
			if (!bTopDown && State.Velocity.Y > 0.0f)
			{
				State.bGrounded   = false;
				State.CoyoteTimer = 0.0f;
				if (State.JumpsUsed == 0)
				{
					State.JumpsUsed = 1; // 바닥을 떠남 = 바닥 점프는 쓴 것으로 (공중 점프는 남는다)
				}
			}
			const float Stun = std::isfinite(Move.StunSeconds) ? std::clamp(Move.StunSeconds, 0.0f, FCharacterMove2D::MaxStunSeconds) : 0.0f;
			State.StunTimer  = std::max(State.StunTimer, Stun);
		}
	} // namespace

	void BeginMove(const FCharacterMovement2DComponent& Movement, FCharacterState2D& State, const FCharacterMove2D& InMove, float GravityZ,
	               const FVector2& GroundVelocity, FCharacterMove2DEvents& OutEvents)
	{
		const float DeltaSeconds = std::clamp(std::isfinite(InMove.DeltaSeconds) ? InMove.DeltaSeconds : 0.0f, 0.0f, FCharacterMove2D::MaxMoveDeltaSeconds);
		const bool  bTopDown     = Movement.Mode == ECharacterMovement2DMode::TopDown;

		// ---- 발사/넉백 + 경직 (경직 중 무브는 입력이 없는 것으로)
		if (InMove.bLaunch)
		{
			ApplyLaunch(State, InMove, bTopDown);
		}
		const bool bStunned = State.IsStunned();
		State.StunTimer     = State.StunTimer - DeltaSeconds > 1.0e-4f ? State.StunTimer - DeltaSeconds : 0.0f; // 프레임 dt 누적 오차 (대시와 같은 여유)
		FCharacterMove2D Move = InMove;
		if (bStunned)
		{
			Move.Input        = FVector2();
			Move.bJumpPressed = false;
			Move.bDash        = false;
			Move.bDropDown    = false;
		}
		const float    KnockbackDecel = std::isfinite(Movement.KnockbackDeceleration) ? std::max(Movement.KnockbackDeceleration, 0.0f) : 0.0f;
		const FVector2 Input          = ClampInput(Move.Input, Movement.Mode);
		const float    MaxSpeed     = std::max(Movement.MaxSpeed, 0.0f);
		if (bTopDown)
		{
			State.bGrounded = true; // 탑다운은 항상 "바닥" (점프·코요테 없음)
		}

		// ---- 타이머
		State.DashCooldownTimer = std::max(State.DashCooldownTimer - DeltaSeconds, 0.0f);
		State.DropTimer         = std::max(State.DropTimer - DeltaSeconds, 0.0f);
		if (Move.bDropDown && !bTopDown)
		{
			State.DropTimer = std::max(Movement.DropThroughTime, 0.0f);
		}
		if (State.bGrounded)
		{
			State.CoyoteTimer = std::max(Movement.CoyoteTime, 0.0f);
		}
		else
		{
			State.CoyoteTimer = std::max(State.CoyoteTimer - DeltaSeconds, 0.0f);
			if (State.CoyoteTimer <= 0.0f && State.JumpsUsed == 0)
			{
				State.JumpsUsed = 1; // 걸어서 떨어짐: 바닥 점프는 쓴 것으로 친다
			}
		}
		const bool bJumpBuffered = !bTopDown && !bStunned && (Move.bJumpPressed || State.JumpBufferTimer > 0.0f);
		if (Move.bJumpPressed && !bTopDown)
		{
			State.JumpBufferTimer = std::max(Movement.JumpBufferTime, 0.0f);
		}

		// ---- 대시
		const bool bCanDash = Move.bDash && State.DashCooldownTimer <= 0.0f && !State.IsDashing() &&
		                      (State.bGrounded || static_cast<int32>(State.AirDashesUsed) < Movement.MaxAirDashes) && Movement.DashTime > 0.0f;
		const float DashSpeed = std::clamp(Movement.DashSpeed, 0.0f, FCharacterMove2D::MaxDashSpeed);
		if (bCanDash)
		{
			State.DashDirection     = ResolveDashDirection(Movement, State, Move);
			State.DashTimer         = Movement.DashTime;
			State.DashCooldownTimer = std::max(Movement.DashCooldown, 0.0f);
			if (!State.bGrounded)
			{
				++State.AirDashesUsed;
			}
			State.bJumpCutAvailable = false;
			OutEvents.bDashStarted  = true;
		}
		if (State.IsDashing())
		{
			State.DashTimer -= DeltaSeconds;
			State.Velocity = State.DashDirection * DashSpeed;
			if (!Movement.bDashIgnoresGravity && !bTopDown)
			{
				State.Velocity.Y += GravityZ * Movement.GravityScale * DeltaSeconds;
			}
			if (State.DashTimer <= 1.0e-4f) // 프레임 dt 누적 오차 (0.15초 = 9프레임)
			{
				State.DashTimer = 0.0f;
				State.Velocity  = State.DashDirection * std::min(MaxSpeed, DashSpeed); // 대시 끝: 보통 속력으로
			}
			State.JumpBufferTimer = std::max(State.JumpBufferTimer - DeltaSeconds, 0.0f);
			return;
		}

		if (bTopDown)
		{
			const FVector2 Target = Input * MaxSpeed;
			const float    Rate   = bStunned ? KnockbackDecel : Target.LengthSquared() > 1.0e-6f ? Movement.GroundAcceleration : Movement.GroundDeceleration;
			State.Velocity        = ApproachVector(State.Velocity, Target, std::max(Rate, 0.0f) * DeltaSeconds);
			return;
		}

		// ---- 플랫포머 수평
		const float Target   = Input.X * MaxSpeed;
		const float Current  = State.Velocity.X;
		const float Accel    = State.bGrounded ? Movement.GroundAcceleration : Movement.AirAcceleration;
		const float Decel    = State.bGrounded ? Movement.GroundDeceleration : Movement.AirDeceleration;
		float       Rate     = bStunned ? KnockbackDecel : Decel;
		if (!bStunned && std::abs(Target) > 1.0e-4f)
		{
			if (Target * Current < 0.0f)
			{
				Rate = std::max(Accel, Decel); // 방향 전환
			}
			else if (std::abs(Target) > std::abs(Current))
			{
				Rate = Accel;
			}
		}
		State.Velocity.X = Approach(Current, Target, std::max(Rate, 0.0f) * DeltaSeconds);

		// ---- 점프
		bool bJumped = false;
		if (bJumpBuffered)
		{
			const bool bGroundJump = (State.bGrounded || State.CoyoteTimer > 0.0f) && State.JumpsUsed == 0;
			if (bGroundJump || static_cast<int32>(State.JumpsUsed) < Movement.MaxJumps)
			{
				State.JumpsUsed         = static_cast<uint8>(std::min<int32>(State.JumpsUsed + 1, 255));
				State.Velocity.Y        = Movement.JumpVelocity;
				if (State.bGrounded || bGroundJump)
				{
					// 움직이는 발판에서 뛰면 발판 속도를 물려받는다 (수평 전부, 수직은 위로 갈 때만)
					State.Velocity.X += GroundVelocity.X;
					State.Velocity.Y += std::max(GroundVelocity.Y, 0.0f);
				}
				State.JumpBufferTimer   = 0.0f;
				State.CoyoteTimer       = 0.0f;
				State.bGrounded         = false;
				State.bJumpCutAvailable = true;
				OutEvents.bJumped       = true;
				OutEvents.JumpIndex     = State.JumpsUsed;
				bJumped                 = true;
			}
		}
		if (!bJumped)
		{
			State.JumpBufferTimer = std::max(State.JumpBufferTimer - DeltaSeconds, 0.0f);
		}

		// ---- 가변 점프 + 중력
		if (State.bJumpCutAvailable)
		{
			if (State.Velocity.Y <= 0.0f)
			{
				State.bJumpCutAvailable = false;
			}
			else if (!Move.bJumpHeld && !bJumped)
			{
				State.Velocity.Y *= std::clamp(Movement.JumpCutFactor, 0.0f, 1.0f);
				State.bJumpCutAvailable = false;
			}
		}
		if (State.bGrounded)
		{
			State.Velocity.Y = 0.0f;
		}
		else
		{
			State.Velocity.Y += GravityZ * Movement.GravityScale * DeltaSeconds;
			State.Velocity.Y = std::max(State.Velocity.Y, -std::abs(Movement.MaxFallSpeed));
		}
	}

	void EndMove(const FCharacterMovement2DComponent& Movement, FCharacterState2D& State, bool bGroundedNow, FCharacterMove2DEvents& OutEvents)
	{
		if (Movement.Mode == ECharacterMovement2DMode::TopDown)
		{
			State.bGrounded     = true;
			State.AirDashesUsed = 0;
			return;
		}
		const bool bWasGrounded = State.bGrounded;
		State.bGrounded         = bGroundedNow;
		if (!bGroundedNow)
		{
			return;
		}
		if (!bWasGrounded)
		{
			OutEvents.bLanded = true;
		}
		State.JumpsUsed         = 0;
		State.AirDashesUsed     = 0;
		State.bJumpCutAvailable = false;
		if (!State.IsDashing())
		{
			State.Velocity.Y = 0.0f;
		}
	}
} // namespace CharacterMovement2DMath

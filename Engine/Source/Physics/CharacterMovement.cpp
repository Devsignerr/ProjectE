#include "Physics/CharacterMovement.h"

#include <algorithm>
#include <cmath>

namespace CharacterMovementMath
{
	FVector2 ClampInput(const FVector2& Input)
	{
		const float LengthSquared = Input.X * Input.X + Input.Y * Input.Y;
		if (!std::isfinite(LengthSquared) || LengthSquared <= 1.0e-8f)
		{
			return FVector2(0.0f, 0.0f);
		}
		if (LengthSquared <= 1.0f)
		{
			return Input;
		}
		const float InvLength = 1.0f / std::sqrt(LengthSquared);
		return FVector2(Input.X * InvLength, Input.Y * InvLength);
	}

	FVector3 ComputeVelocity(const FCharacterMovementComponent& Movement, const FVector3& CurrentVelocity, bool bGrounded, const FCharacterMove& Move,
	                         float GravityZ, bool& bOutJumped)
	{
		const float    DeltaSeconds = std::clamp(Move.DeltaSeconds, 0.0f, FCharacterMove::MaxMoveDeltaSeconds);
		const FVector2 Input        = ClampInput(Move.Input);
		const FVector2 Desired(Input.X * Movement.MaxWalkSpeed, Input.Y * Movement.MaxWalkSpeed);

		FVector3 Velocity = CurrentVelocity;
		bOutJumped        = false;
		if (bGrounded)
		{
			Velocity.X = Desired.X;
			Velocity.Y = Desired.Y;
			Velocity.Z = 0.0f;
			if (Move.bJump)
			{
				Velocity.Z = Movement.JumpZVelocity;
				bOutJumped = true;
			}
		}
		else
		{
			// 공중: 초당 (AirControl × 최대 속도 × 4)까지 입력 쪽으로 가속 — AirControl 1이면 0.25초에 최고 속도 방향 전환
			const float MaxDelta = std::clamp(Movement.AirControl, 0.0f, 1.0f) * Movement.MaxWalkSpeed * 4.0f * DeltaSeconds;
			FVector2    Delta(Desired.X - Velocity.X, Desired.Y - Velocity.Y);
			const float DeltaLength = std::sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y);
			if (DeltaLength > MaxDelta && DeltaLength > 0.0f)
			{
				Delta = FVector2(Delta.X * (MaxDelta / DeltaLength), Delta.Y * (MaxDelta / DeltaLength));
			}
			Velocity.X += Delta.X;
			Velocity.Y += Delta.Y;
		}
		Velocity.Z += GravityZ * Movement.GravityScale * DeltaSeconds;
		return Velocity;
	}
} // namespace CharacterMovementMath

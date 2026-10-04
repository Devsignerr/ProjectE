#include "Physics/CharacterMovement.h"

#include "Scene/AnimationSystem.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float MaxPendingRootMotionSeconds = 0.25f;

	// 루트 모션 수신 (FAnimationSystem): 애니메이션 엔티티 자신 또는 가장 가까운 조상의 캐릭터 이동 컴포넌트에 쌓는다
	bool ReceiveRootMotion(FScene& Scene, FEntity AnimEntity, const FVector3& WorldTranslation, float WorldYaw, float DeltaSeconds)
	{
		(void)WorldYaw; // 캐릭터 몸 방향은 무브 Yaw 규칙을 따른다 (CharacterMovement.h)
		FRegistry& Registry = Scene.GetRegistry();
		for (FEntity Current = AnimEntity; Current.IsValid() && Registry.IsValid(Current); Current = Scene.GetParent(Current))
		{
			if (FCharacterMovementComponent* Movement = Registry.TryGet<FCharacterMovementComponent>(Current))
			{
				if (Movement->PendingRootMotionSeconds > MaxPendingRootMotionSeconds)
				{
					// 아무도 가져가지 않음 (편집 모드/원격 캐릭터) — 오래된 것은 버린다
					Movement->PendingRootMotion        = FVector3();
					Movement->PendingRootMotionSeconds = 0.0f;
				}
				Movement->PendingRootMotion        = Movement->PendingRootMotion + WorldTranslation;
				Movement->PendingRootMotionSeconds += std::max(DeltaSeconds, 0.0f);
				return true;
			}
		}
		return false;
	}

	[[maybe_unused]] const bool GRootMotionReceiverRegistered = [] {
		FAnimationSystem::SetRootMotionReceiver(&ReceiveRootMotion);
		return true;
	}();
} // namespace

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

	FVector2 ClampRootMotionVelocity(const FVector2& Velocity)
	{
		const float LengthSquared = Velocity.X * Velocity.X + Velocity.Y * Velocity.Y;
		if (!std::isfinite(LengthSquared))
		{
			return FVector2(0.0f, 0.0f);
		}
		constexpr float Max = FCharacterMove::MaxRootMotionSpeed;
		if (LengthSquared <= Max * Max)
		{
			return Velocity;
		}
		const float Scale = Max / std::sqrt(LengthSquared);
		return FVector2(Velocity.X * Scale, Velocity.Y * Scale);
	}

	void ConsumeRootMotion(FCharacterMovementComponent& Movement, FCharacterMove& Move)
	{
		if (Movement.PendingRootMotionSeconds <= 0.0f)
		{
			return;
		}
		Move.bRootMotion        = true;
		Move.RootMotionVelocity = ClampRootMotionVelocity(FVector2(Movement.PendingRootMotion.X / Movement.PendingRootMotionSeconds,
		                                                           Movement.PendingRootMotion.Y / Movement.PendingRootMotionSeconds));
		Movement.PendingRootMotion        = FVector3();
		Movement.PendingRootMotionSeconds = 0.0f;
	}

	namespace
	{
		float ClampLaunchComponent(float Value)
		{
			return std::isfinite(Value) ? std::clamp(Value, -FCharacterMove::MaxLaunchSpeed, FCharacterMove::MaxLaunchSpeed) : 0.0f;
		}

		float ClampStunSeconds(float Value)
		{
			return std::isfinite(Value) ? std::clamp(Value, 0.0f, FCharacterMove::MaxStunSeconds) : 0.0f;
		}
	} // namespace

	void CombineLaunch(FCharacterMove& Into, const FVector3& Velocity, bool bOverrideXY, bool bOverrideZ, float StunSeconds)
	{
		if (!Into.bLaunch)
		{
			Into.bLaunch           = true;
			Into.LaunchVelocity    = FVector3();
			Into.bLaunchOverrideXY = false;
			Into.bLaunchOverrideZ  = false;
			Into.StunSeconds       = 0.0f;
		}
		if (bOverrideXY)
		{
			Into.LaunchVelocity.X = Velocity.X;
			Into.LaunchVelocity.Y = Velocity.Y;
		}
		else
		{
			Into.LaunchVelocity.X += Velocity.X;
			Into.LaunchVelocity.Y += Velocity.Y;
		}
		Into.LaunchVelocity.Z  = bOverrideZ ? Velocity.Z : Into.LaunchVelocity.Z + Velocity.Z;
		Into.bLaunchOverrideXY = Into.bLaunchOverrideXY || bOverrideXY;
		Into.bLaunchOverrideZ  = Into.bLaunchOverrideZ || bOverrideZ;
		Into.StunSeconds       = std::max(Into.StunSeconds, ClampStunSeconds(StunSeconds));
	}

	bool SanitizeLaunch(FCharacterMove& Move)
	{
		if (!Move.bLaunch)
		{
			return true;
		}
		if (!std::isfinite(Move.LaunchVelocity.X) || !std::isfinite(Move.LaunchVelocity.Y) || !std::isfinite(Move.LaunchVelocity.Z) ||
		    !std::isfinite(Move.StunSeconds))
		{
			return false;
		}
		Move.LaunchVelocity = FVector3(ClampLaunchComponent(Move.LaunchVelocity.X), ClampLaunchComponent(Move.LaunchVelocity.Y),
		                               ClampLaunchComponent(Move.LaunchVelocity.Z));
		Move.StunSeconds    = ClampStunSeconds(Move.StunSeconds);
		return true;
	}

	FVector3 ComputeVelocity(const FCharacterMovementComponent& Movement, const FVector3& CurrentVelocity, bool bGrounded, const FCharacterMove& Move,
	                         float GravityZ, bool& bOutJumped)
	{
		float StunTimer   = 0.0f;
		bool  bAirborne   = false;
		return ComputeVelocity(Movement, CurrentVelocity, bGrounded, Move, GravityZ, StunTimer, bOutJumped, bAirborne);
	}

	FVector3 ComputeVelocity(const FCharacterMovementComponent& Movement, const FVector3& CurrentVelocity, bool bInGrounded, const FCharacterMove& Move,
	                         float GravityZ, float& InOutStunTimer, bool& bOutJumped, bool& bOutAirborne)
	{
		const float DeltaSeconds = std::clamp(std::isfinite(Move.DeltaSeconds) ? Move.DeltaSeconds : 0.0f, 0.0f, FCharacterMove::MaxMoveDeltaSeconds);
		FVector3    Velocity     = CurrentVelocity;
		bool        bGrounded    = bInGrounded;
		bOutJumped               = false;
		bOutAirborne             = false;

		// ---- 발사/넉백 (가장 먼저 — 헤더 주석)
		if (Move.bLaunch)
		{
			const FVector3 Launch(ClampLaunchComponent(Move.LaunchVelocity.X), ClampLaunchComponent(Move.LaunchVelocity.Y),
			                      ClampLaunchComponent(Move.LaunchVelocity.Z));
			Velocity.X     = Move.bLaunchOverrideXY ? Launch.X : Velocity.X + Launch.X;
			Velocity.Y     = Move.bLaunchOverrideXY ? Launch.Y : Velocity.Y + Launch.Y;
			Velocity.Z     = Move.bLaunchOverrideZ ? Launch.Z : (bGrounded ? std::max(Velocity.Z, 0.0f) : Velocity.Z) + Launch.Z;
			InOutStunTimer = std::max(std::isfinite(InOutStunTimer) ? InOutStunTimer : 0.0f, ClampStunSeconds(Move.StunSeconds));
			bGrounded      = false; // 이 무브는 공중 규칙 (바닥 붙이기 없음)
			bOutAirborne   = true;
		}

		// ---- 경직: 입력·점프·루트 모션 무시, 수평 감속
		const bool bStunned = std::isfinite(InOutStunTimer) && InOutStunTimer > 0.0f;
		InOutStunTimer      = bStunned && InOutStunTimer - DeltaSeconds > 1.0e-4f ? InOutStunTimer - DeltaSeconds : 0.0f; // 프레임 dt 누적 오차 여유 (2D와 같음)
		if (bStunned)
		{
			const float Decel  = std::isfinite(Movement.KnockbackDeceleration) ? std::max(Movement.KnockbackDeceleration, 0.0f) : 0.0f;
			const float Speed  = std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y);
			const float Reduce = Decel * DeltaSeconds;
			if (Speed <= Reduce || Speed <= 0.0f)
			{
				Velocity.X = 0.0f;
				Velocity.Y = 0.0f;
			}
			else
			{
				const float Scale = (Speed - Reduce) / Speed;
				Velocity.X *= Scale;
				Velocity.Y *= Scale;
			}
			if (bGrounded)
			{
				Velocity.Z = 0.0f;
			}
			Velocity.Z += GravityZ * Movement.GravityScale * DeltaSeconds;
			return Velocity;
		}

		if (Move.bRootMotion)
		{
			// 루트 모션: 수평은 애니메이션 속도 그대로 (충돌/계단/바닥 붙이기는 Jolt 이동이 맡는다)
			const FVector2 RootMotion = ClampRootMotionVelocity(Move.RootMotionVelocity);
			Velocity.X                = RootMotion.X;
			Velocity.Y                = RootMotion.Y;
			if (bGrounded)
			{
				Velocity.Z = 0.0f;
			}
			Velocity.Z += GravityZ * Movement.GravityScale * DeltaSeconds;
			return Velocity;
		}

		const FVector2 Input = ClampInput(Move.Input);
		const FVector2 Desired(Input.X * Movement.MaxWalkSpeed, Input.Y * Movement.MaxWalkSpeed);
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

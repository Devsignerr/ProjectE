#include "Renderer/Camera.h"

void FCamera::LookAt(const FVector3& Target)
{
	const FVector3 Direction = (Target - Position).GetNormalized();
	if (Direction.IsNearlyZero())
	{
		return;
	}

	// 방향 → Yaw(Z축), Pitch(위쪽 양수)
	const float YawDegrees   = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
	const float PitchDegrees = FMath::RadiansToDegrees(FMath::Asin(Direction.Z));
	Rotation = FQuat::FromEuler(PitchDegrees, YawDegrees, 0.0f);
}

void FCamera::SetPerspective(float InFovYDegrees, float InAspectRatio, float InNearZ, float InFarZ)
{
	FovYDegrees = InFovYDegrees;
	AspectRatio = InAspectRatio;
	NearZ       = InNearZ;
	FarZ        = InFarZ;
}

FMatrix4x4 FCamera::GetViewMatrix() const
{
	return FMatrix4x4::MakeLookAt(Position, Position + GetForwardVector(), GetUpVector());
}

FMatrix4x4 FCamera::GetProjectionMatrix() const
{
	return FMatrix4x4::MakePerspectiveFov(FMath::DegreesToRadians(FovYDegrees), AspectRatio, NearZ, FarZ);
}

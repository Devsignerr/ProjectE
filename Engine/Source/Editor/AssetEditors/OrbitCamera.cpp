#include "Editor/AssetEditors/OrbitCamera.h"

#include "Editor/EditorCameraState.h"
#include "Renderer/Camera.h"

#include <cmath>

void FOrbitCamera::Orbit(float DeltaYawDegrees, float DeltaPitchDegrees)
{
	Yaw   = std::fmod(Yaw + DeltaYawDegrees, 360.0f);
	Pitch = FMath::Clamp(Pitch + DeltaPitchDegrees, MinPitch, MaxPitch);
}

void FOrbitCamera::Zoom(float WheelSteps)
{
	Distance = FMath::Clamp(Distance * std::pow(0.85f, WheelSteps), MinDistance, MaxDistance);
}

void FOrbitCamera::Pan(float DeltaPixelsX, float DeltaPixelsY, float FovYDegrees, float ViewportHeightPixels)
{
	if (ViewportHeightPixels <= 0.0f)
	{
		return;
	}
	// Target 거리에서 화면 1픽셀이 차지하는 월드 길이
	const float WorldPerPixel = 2.0f * Distance * FMath::Tan(FMath::DegreesToRadians(FovYDegrees) * 0.5f) / ViewportHeightPixels;
	const FQuat Rotation      = GetRotation();
	Target -= Rotation.GetRightVector() * (DeltaPixelsX * WorldPerPixel);
	Target += Rotation.GetUpVector() * (DeltaPixelsY * WorldPerPixel);
}

void FOrbitCamera::Frame(const FBox& Bounds, float FovYDegrees, float AspectRatio)
{
	if (!Bounds.IsValid())
	{
		return;
	}
	const FVector3 Forward  = GetRotation().GetForwardVector();
	const FVector3 Position = FEditorCameraState::ComputeFramingPosition(Bounds, Forward, FovYDegrees, AspectRatio);
	Target                  = Bounds.GetCenter();
	Distance                = FMath::Clamp((Target - Position).Length(), MinDistance, MaxDistance);
}

FQuat FOrbitCamera::GetRotation() const
{
	return FQuat::FromEuler(Pitch, Yaw, 0.0f);
}

FVector3 FOrbitCamera::GetPosition() const
{
	return Target - GetRotation().GetForwardVector() * Distance;
}

void FOrbitCamera::ApplyTo(FCamera& Camera) const
{
	Camera.SetPosition(GetPosition());
	Camera.SetRotation(GetRotation());
}

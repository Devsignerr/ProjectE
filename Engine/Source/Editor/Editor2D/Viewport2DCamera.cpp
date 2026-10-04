#include "Editor/Editor2D/Viewport2DCamera.h"

#include "Editor/Editor2D/Editor2DMath.h"
#include "Editor/EditorCameraState.h"
#include "Renderer/Camera.h"

#include <cmath>

namespace
{
	FVector2 GetPlane(const FCamera& Camera) { return FVector2(Camera.GetPosition().X, Camera.GetPosition().Z); }

	void SetPlane(FCamera& Camera, const FVector2& Plane)
	{
		Camera.SetPosition(FVector3(Plane.X, FViewport2DCamera::CameraDepth, Plane.Y));
	}

	void SetHeight(FCamera& Camera, float Height)
	{
		Camera.SetOrthographic(FMath::Clamp(Height, FViewport2DCamera::MinOrthoHeight, FViewport2DCamera::MaxOrthoHeight), Camera.GetAspectRatio(),
		                       Camera.GetNearZ(), Camera.GetFarZ());
	}
} // namespace

void FViewport2DCamera::Enter(FCamera& Camera)
{
	if (bEnabled)
	{
		return;
	}
	bHasSaved3D          = true;
	Saved3DPosition      = Camera.GetPosition();
	Saved3DRotation      = Camera.GetRotation();
	bSaved3DOrthographic = Camera.IsOrthographic();
	Saved3DOrthoHeight   = Camera.GetOrthoHeight();

	// 화면 가운데에 올 평면 점: 시선이 Y = 0 평면에 닿는 곳 (멀면 카메라 X/Z)
	const FVector3 Position = Camera.GetPosition();
	const FVector3 Forward  = Camera.GetForwardVector();
	FVector2       Center(Position.X, Position.Z);
	float          Height   = Camera.IsOrthographic() ? Camera.GetOrthoHeight() : 1000.0f;
	if (std::abs(Forward.Y) > 0.05f)
	{
		const float Distance = -Position.Y / Forward.Y;
		if (Distance > 0.0f && Distance < 50000.0f)
		{
			const FVector3 Hit = Position + Forward * Distance;
			Center             = FVector2(Hit.X, Hit.Z);
			if (!Camera.IsOrthographic())
			{
				Height = FEditorCameraState::ComputeMatchingOrthoHeight(Camera.GetFovYDegrees(), Distance);
			}
		}
	}
	bEnabled = true;
	Camera.SetRotation(Editor2DMath::Get2DCameraRotation());
	SetPlane(Camera, Center);
	SetHeight(Camera, Height);
}

void FViewport2DCamera::Exit(FCamera& Camera)
{
	if (!bEnabled)
	{
		return;
	}
	bEnabled = false;
	if (!bHasSaved3D)
	{
		Camera.SetPerspectiveMode();
		return;
	}
	Camera.SetPosition(Saved3DPosition);
	Camera.SetRotation(Saved3DRotation);
	if (bSaved3DOrthographic)
	{
		Camera.SetOrthographic(Saved3DOrthoHeight, Camera.GetAspectRatio(), Camera.GetNearZ(), Camera.GetFarZ());
	}
	else
	{
		Camera.SetPerspectiveMode();
	}
}

void FViewport2DCamera::Enforce(FCamera& Camera) const
{
	if (!bEnabled)
	{
		return;
	}
	Camera.SetRotation(Editor2DMath::Get2DCameraRotation());
	if (Camera.GetPosition().Y != CameraDepth)
	{
		SetPlane(Camera, GetPlane(Camera));
	}
	if (!Camera.IsOrthographic())
	{
		SetHeight(Camera, Camera.GetOrthoHeight());
	}
}

void FViewport2DCamera::Zoom(FCamera& Camera, float Wheel, const FVector2& Pixel, const FVector2& ImageSize) const
{
	if (Wheel == 0.0f || ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return;
	}
	const float OldHeight = Camera.GetOrthoHeight();
	const float NewHeight = FMath::Clamp(OldHeight * std::pow(1.15f, -Wheel), MinOrthoHeight, MaxOrthoHeight);
	SetPlane(Camera, Editor2DMath::ZoomAroundCursor(GetPlane(Camera), OldHeight, NewHeight, ImageSize, Pixel));
	SetHeight(Camera, NewHeight);
}

void FViewport2DCamera::Pan(FCamera& Camera, const FVector2& PanDelta, const FVector2& ImageSize) const
{
	if (ImageSize.Y <= 0.0f || (PanDelta.X == 0.0f && PanDelta.Y == 0.0f))
	{
		return;
	}
	// 화면을 끈 방향으로 세상이 따라온다: 카메라는 반대로 (오른쪽으로 끌면 카메라 -X, 아래로 끌면 카메라 +Z)
	const float UnitsPerPixel = Camera.GetOrthoHeight() / ImageSize.Y;
	SetPlane(Camera, GetPlane(Camera) + FVector2(-PanDelta.X, PanDelta.Y) * UnitsPerPixel);
}

void FViewport2DCamera::FitBounds(FCamera& Camera, const FBox& Bounds) const
{
	if (!Bounds.IsValid())
	{
		return;
	}
	const FVector3 Center = Bounds.GetCenter();
	const FVector3 Size   = Bounds.Max - Bounds.Min;
	const float    Aspect = FMath::Max(Camera.GetAspectRatio(), 0.01f);
	SetPlane(Camera, FVector2(Center.X, Center.Z));
	SetHeight(Camera, FMath::Max(FMath::Max(Size.Z, Size.X / Aspect) * 1.25f, 100.0f));
}

void FViewport2DCamera::WriteState(FEditorCameraState& State) const
{
	State.bViewport2D          = bEnabled;
	State.bHasSaved3D          = bEnabled && bHasSaved3D;
	State.Saved3DPosition      = Saved3DPosition;
	State.Saved3DRotation      = Saved3DRotation;
	State.bSaved3DOrthographic = bSaved3DOrthographic;
	State.Saved3DOrthoHeight   = Saved3DOrthoHeight;
}

void FViewport2DCamera::ReadState(const FEditorCameraState& State, FCamera& Camera)
{
	bEnabled             = State.bViewport2D;
	bHasSaved3D          = State.bHasSaved3D;
	Saved3DPosition      = State.Saved3DPosition;
	Saved3DRotation      = State.Saved3DRotation;
	bSaved3DOrthographic = State.bSaved3DOrthographic;
	Saved3DOrthoHeight   = State.Saved3DOrthoHeight;
	Enforce(Camera);
}

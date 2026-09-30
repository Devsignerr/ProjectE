#include "Renderer/FlyCameraController.h"

#include "Core/Input.h"
#include "Renderer/Camera.h"

void FFlyCameraController::Update(FCamera& Camera, const FInput& Input, float DeltaSeconds)
{
	if (!bSynced)
	{
		SyncFromCamera(Camera);
	}

	if (!Input.IsMouseButtonDown(EMouseButton::Right))
	{
		// 직교 줌: 한 눈금당 15%
		const float ZoomWheel = Input.GetMouseWheelDelta();
		if (Camera.IsOrthographic() && ZoomWheel != 0.0f)
		{
			const float Height = FMath::Clamp(Camera.GetOrthoHeight() * std::pow(1.15f, -ZoomWheel), MinOrthoHeight, MaxOrthoHeight);
			Camera.SetOrthographic(Height, Camera.GetAspectRatio(), Camera.GetNearZ(), Camera.GetFarZ());
		}
		return;
	}

	// 휠: 이동 속도 조절 (한 눈금당 20%)
	const float Wheel = Input.GetMouseWheelDelta();
	if (Wheel != 0.0f)
	{
		MoveSpeed = FMath::Clamp(MoveSpeed * std::pow(1.2f, Wheel), 10.0f, 50000.0f);
	}

	// 회전: 화면 아래로 드래그하면 Pitch 감소(아래를 봄)
	YawDegrees += static_cast<float>(Input.GetMouseDeltaX()) * LookSensitivity;
	PitchDegrees = FMath::Clamp(PitchDegrees - static_cast<float>(Input.GetMouseDeltaY()) * LookSensitivity,
	                            -MaxPitchDegrees, MaxPitchDegrees);
	Camera.SetRotation(FQuat::FromEuler(PitchDegrees, YawDegrees, 0.0f));

	const float Speed = MoveSpeed * (Input.IsKeyDown(EKey::LeftShift) ? FastMultiplier : 1.0f);

	// 이동. 직교는 시선 방향으로 움직여도 화면이 변하지 않으므로 W/S를 같은 높이(Z)에서 수평 이동으로 쓴다
	//   (시선을 바닥에 투영한 방향, 바로 아래를 보고 있으면 화면 위쪽 방향)
	FVector3 ForwardMove = Camera.GetForwardVector();
	if (Camera.IsOrthographic())
	{
		const FVector3 Flat = FVector3(ForwardMove.X, ForwardMove.Y, 0.0f);
		ForwardMove         = Flat.IsNearlyZero() ? FVector3(Camera.GetUpVector().X, Camera.GetUpVector().Y, 0.0f).GetNormalized() : Flat.GetNormalized();
	}
	FVector3 Direction;
	if (Input.IsKeyDown(EKey::W)) Direction += ForwardMove;
	if (Input.IsKeyDown(EKey::S)) Direction -= ForwardMove;
	if (Input.IsKeyDown(EKey::D)) Direction += Camera.GetRightVector();
	if (Input.IsKeyDown(EKey::A)) Direction -= Camera.GetRightVector();
	if (Input.IsKeyDown(EKey::E)) Direction += FVector3::UpVector;
	if (Input.IsKeyDown(EKey::Q)) Direction -= FVector3::UpVector;

	if (!Direction.IsNearlyZero())
	{
		Camera.SetPosition(Camera.GetPosition() + Direction.GetNormalized() * (Speed * DeltaSeconds));
	}
}

void FFlyCameraController::SyncFromCamera(const FCamera& Camera)
{
	const FVector3 Forward = Camera.GetForwardVector();
	YawDegrees   = FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X));
	PitchDegrees = FMath::RadiansToDegrees(FMath::Asin(Forward.Z));
	bSynced      = true;
}

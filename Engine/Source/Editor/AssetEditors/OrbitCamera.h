#pragma once

#include "Core/Math/Math.h"

class FCamera;

// 에셋 미리보기용 궤도 카메라: Target을 중심으로 Yaw/Pitch(도)와 Distance(cm)로 위치를 정한다.
// Pitch 음수 = 위에서 내려다봄 (FQuat::FromEuler 부호 규약)
struct FOrbitCamera
{
	FVector3 Target;
	float    Yaw      = 35.0f;
	float    Pitch    = -20.0f;
	float    Distance = 300.0f;

	static constexpr float MinPitch    = -89.0f;
	static constexpr float MaxPitch    = 89.0f;
	static constexpr float MinDistance = 1.0f;
	static constexpr float MaxDistance = 100000.0f;

	void Orbit(float DeltaYawDegrees, float DeltaPitchDegrees);
	// 휠 한 칸당 15%씩 가까워진다 (음수면 멀어짐)
	void Zoom(float WheelSteps);
	// 화면 픽셀 이동량만큼 Target을 카메라 평면에서 옮긴다 (드래그한 점이 커서를 따라오도록)
	void Pan(float DeltaPixelsX, float DeltaPixelsY, float FovYDegrees, float ViewportHeightPixels);
	// Bounds 전체가 보이도록 Target/Distance 설정 (방향 유지)
	void Frame(const FBox& Bounds, float FovYDegrees, float AspectRatio);

	FQuat    GetRotation() const;
	FVector3 GetPosition() const;
	void     ApplyTo(FCamera& Camera) const;
};

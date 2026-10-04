#pragma once

#include "Core/Math/Math.h"

class FCamera;
struct FEditorCameraState;

// 뷰포트 2D 모드 편집 카메라 (Phase 56-5a): 직교, +Y 쪽(Y = CameraDepth)에서 -Y를 보는 회전 없는 카메라.
//   휠 = 커서 기준 확대/축소(직교 높이), 가운데/오른쪽 드래그 = 팬, F = 선택 맞춤(FitBounds). 3D 카메라는 들어갈 때 저장하고 나올 때 되살린다.
class FViewport2DCamera
{
public:
	static constexpr float CameraDepth    = 5000.0f; // cm (2D 카메라 월드 Y — 스프라이트·타일맵은 보통 Y ±수백)
	static constexpr float MinOrthoHeight = 20.0f;
	static constexpr float MaxOrthoHeight = 200000.0f;

	bool IsEnabled() const { return bEnabled; }

	// 3D → 2D: 지금 카메라를 저장하고, 시선이 Y = 0 평면에 닿는 점(없으면 카메라 X/Z)을 화면 가운데로
	void Enter(FCamera& Camera);
	// 2D → 3D: 저장한 3D 카메라 (없으면 원근으로만 바꾼다)
	void Exit(FCamera& Camera);
	// 매 프레임 (2D 중): 회전·직교·깊이를 고정 (다른 코드가 바꿔도 되돌린다)
	void Enforce(FCamera& Camera) const;

	// 입력 (뷰포트 위에서만 부른다). Pixel = 이미지 왼쪽 위 기준 커서, Wheel = 눈금, PanDelta = 이번 프레임 드래그 픽셀
	void Zoom(FCamera& Camera, float Wheel, const FVector2& Pixel, const FVector2& ImageSize) const;
	void Pan(FCamera& Camera, const FVector2& PanDelta, const FVector2& ImageSize) const;
	// Bounds(월드)가 화면에 여유 있게 들어오게 (가운데 + 직교 높이)
	void FitBounds(FCamera& Camera, const FBox& Bounds) const;

	// 저장/복원 (EditorCamera.json): 2D 여부 + 저장한 3D 카메라
	void WriteState(FEditorCameraState& State) const;
	void ReadState(const FEditorCameraState& State, FCamera& Camera);

private:
	bool     bEnabled          = false;
	bool     bHasSaved3D       = false;
	FVector3 Saved3DPosition;
	FQuat    Saved3DRotation;
	bool     bSaved3DOrthographic = false;
	float    Saved3DOrthoHeight   = 1000.0f;
};

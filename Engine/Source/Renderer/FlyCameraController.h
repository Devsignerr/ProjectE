#pragma once

#include "Core/CoreTypes.h"

class FCamera;
class FInput;

// 에디터 스타일 자유 시점 조작.
// 마우스 오른쪽 버튼을 누른 동안: 마우스로 회전, WASD 이동, Q/E 아래/위(월드), Shift 가속, 휠로 속도 조절
// 직교 카메라: W/S는 같은 높이에서 수평 이동(시선을 바닥에 투영한 방향), 오른쪽 버튼 없이 휠을 굴리면 확대/축소(직교 높이)
class FFlyCameraController
{
public:
	void Update(FCamera& Camera, const FInput& Input, float DeltaSeconds);

	// 외부에서 카메라 회전을 바꿨을 때 내부 Yaw/Pitch를 다시 맞춘다
	void SyncFromCamera(const FCamera& Camera);

	float MoveSpeed            = 500.0f; // cm/초
	float FastMultiplier       = 4.0f;
	float LookSensitivity      = 0.15f; // 도/픽셀
	float MaxPitchDegrees      = 89.0f;
	float MinOrthoHeight       = 10.0f;      // cm (직교 줌 범위)
	float MaxOrthoHeight       = 1000000.0f;

private:
	float YawDegrees   = 0.0f;
	float PitchDegrees = 0.0f;
	bool  bSynced      = false;
};

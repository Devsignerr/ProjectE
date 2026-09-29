#pragma once

#include "Core/CoreTypes.h"

class FCamera;
class FInput;

// 에디터 스타일 자유 시점 조작.
// 마우스 오른쪽 버튼을 누른 동안: 마우스로 회전, WASD 이동, Q/E 아래/위(월드), Shift 가속, 휠로 속도 조절
class FFlyCameraController
{
public:
	void Update(FCamera& Camera, const FInput& Input, float DeltaSeconds);

	// 외부에서 카메라 회전을 바꿨을 때 내부 Yaw/Pitch를 다시 맞춘다
	void SyncFromCamera(const FCamera& Camera);

	float MoveSpeed            = 5.0f;  // 단위/초
	float FastMultiplier       = 4.0f;
	float LookSensitivity      = 0.15f; // 도/픽셀
	float MaxPitchDegrees      = 89.0f;

private:
	float YawDegrees   = 0.0f;
	float PitchDegrees = 0.0f;
	bool  bSynced      = false;
};

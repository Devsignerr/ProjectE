#pragma once

#include "Core/CoreTypes.h"

// 기즈모 조작 종류
enum class ETransformTool : uint8
{
	Translate,
	Rotate,
	Scale,
};

// 기즈모 스냅 설정. 단위: 이동 cm, 회전 도, 스케일 배율
struct FSnapSettings
{
	bool  bEnabled         = false;
	float TranslateStep    = 10.0f;
	float RotateStepDegree = 15.0f;
	float ScaleStep        = 0.1f;

	// 이번 조작에 적용할 스냅 값을 OutValues(3개)에 쓰고 포인터를 반환한다. 스냅하지 않으면 nullptr.
	// bInvert(예: Ctrl을 누른 동안)는 켜짐/꺼짐을 일시적으로 뒤집는다. ImGuizmo::Manipulate의 snap 인자 형식.
	const float* GetSnapValues(ETransformTool Tool, bool bInvert, float OutValues[3]) const
	{
		if (bEnabled == bInvert)
		{
			return nullptr;
		}
		float Step = TranslateStep;
		switch (Tool)
		{
		case ETransformTool::Rotate: Step = RotateStepDegree; break;
		case ETransformTool::Scale:  Step = ScaleStep; break;
		default:                     break;
		}
		if (Step <= 0.0f)
		{
			return nullptr;
		}
		OutValues[0] = OutValues[1] = OutValues[2] = Step;
		return OutValues;
	}
};

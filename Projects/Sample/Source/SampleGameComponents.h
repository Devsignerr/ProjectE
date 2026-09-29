#pragma once

#include "Core/Math/Math.h"

// Sample 게임 컴포넌트 (SampleGameModule.cpp에서 리플렉션 등록 → 인스펙터/직렬화/Lua에 자동 노출)

// 축을 중심으로 계속 회전
struct FSpinnerComponent
{
	float    DegreesPerSecond = 90.0f;
	FVector3 Axis             = FVector3::UpVector;
};

// 시작 높이를 기준으로 위아래로 흔들림 (Z, cm)
struct FHoverComponent
{
	float Amplitude = 20.0f; // cm
	float Frequency = 0.5f;  // Hz

	// 런타임 상태 (등록하지 않음 → 직렬화 제외)
	float BaseHeight   = 0.0f;
	float Elapsed      = 0.0f;
	bool  bInitialized = false;
};

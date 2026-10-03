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

// 원 궤도를 따라 걷는 유닛 (Tests/Stress — 대량 이동 부하). 중심·반지름·속도로 각도를 적분하고 진행 방향을 본다
struct FStressWalkerComponent
{
	FVector3 Center     = FVector3::ZeroVector; // 궤도 중심 (월드, Z는 무시하고 시작 높이 유지)
	float    Speed      = 150.0f;               // cm/s (음수 = 시계 방향)
	float    YawOffset  = 180.0f;               // 모델 앞 방향 보정 (KayKit = glTF +Z 앞 → 180)

	// 런타임 상태 (등록하지 않음)
	float Radius       = 0.0f;
	float Angle        = 0.0f; // 라디안
	bool  bInitialized = false;
};


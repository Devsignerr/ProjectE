#pragma once

#include "Core/Math/Math.h"

// 셰이더 상수 버퍼와 1:1 대응하는 CPU 구조체.
// HLSL cbuffer 패킹 규칙(16바이트 경계)을 따르도록 필드 순서/패딩을 맞춘다. 행렬은 행우선(-Zpr).

// 방향광 (태양광). Direction은 빛이 진행하는 방향(정규화)
struct alignas(16) FDirectionalLightConstants
{
	FVector3 Direction = FVector3(0.0f, 0.0f, -1.0f);
	float    Intensity = 1.0f;
	FVector3 Color     = FVector3::OneVector;
	float    Padding0  = 0.0f;
};
static_assert(sizeof(FDirectionalLightConstants) == 32);

struct alignas(16) FPerFrameConstants
{
	FMatrix4x4                 ViewProjection;
	FVector3                   CameraPosition;
	float                      Padding0 = 0.0f;
	FDirectionalLightConstants DirectionalLight;
	FVector3                   AmbientColor = FVector3(0.03f, 0.03f, 0.04f);
	float                      Padding1     = 0.0f;
};
static_assert(sizeof(FPerFrameConstants) % 16 == 0);

struct alignas(16) FPerObjectConstants
{
	FMatrix4x4 World;
	FMatrix4x4 WorldInverseTranspose; // 비균등 스케일에서도 올바른 법선 변환
};
static_assert(sizeof(FPerObjectConstants) % 16 == 0);

// Blinn-Phong 머티리얼
struct alignas(16) FMaterialConstants
{
	FVector4 BaseColorTint    = FVector4::OneVector;
	FVector3 SpecularColor    = FVector3(0.04f, 0.04f, 0.04f);
	float    Shininess        = 64.0f; // 스페큘러 지수
	float    SpecularStrength = 1.0f;
	FVector3 Padding0;
};
static_assert(sizeof(FMaterialConstants) % 16 == 0);

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
	// 간이 환경광 (하늘/지면 반구). 이후 IBL이 대체한다
	FVector3                   SkyColor         = FVector3(0.35f, 0.45f, 0.6f);
	float                      AmbientIntensity = 1.0f;
	FVector3                   GroundColor      = FVector3(0.15f, 0.13f, 0.1f);
	float                      Padding1         = 0.0f;
};
static_assert(sizeof(FPerFrameConstants) % 16 == 0);

struct alignas(16) FPerObjectConstants
{
	FMatrix4x4 World;
	FMatrix4x4 WorldInverseTranspose; // 비균등 스케일에서도 올바른 법선 변환
};
static_assert(sizeof(FPerObjectConstants) % 16 == 0);

// 금속/거칠기 PBR 머티리얼 (glTF 2.0 규약). 텍스처 값에 곱해지는 팩터들
struct alignas(16) FMaterialConstants
{
	FVector4 BaseColorFactor   = FVector4::OneVector;
	FVector3 EmissiveFactor    = FVector3::ZeroVector; // HDR 선형
	float    Metallic          = 0.0f;
	float    Roughness         = 0.5f;
	float    NormalScale       = 1.0f; // 노멀 맵 XY 배율
	float    OcclusionStrength = 1.0f; // 0 = AO 무시, 1 = 전체 적용
	float    AlphaCutoff       = 0.5f; // 알파 테스트(후속)
};
static_assert(sizeof(FMaterialConstants) == 48);

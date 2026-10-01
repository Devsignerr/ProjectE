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

// 메시 인스턴스 하나 (구조화 버퍼 t13, MeshInstance.hlsli FInstanceData와 1:1).
// 패스는 인스턴스 번호 목록(t14)의 [InstanceOffset, + 인스턴스 수) 구간을 DrawIndexedInstanced로 그린다
// 스킨 메시는 World/NormalMatrix 대신 BoneOffset(프레임 팔레트 버퍼 t15 안 첫 본 행렬 번호)을 쓴다
struct FInstanceGpuData
{
	FMatrix4x4 World;
	FVector4   NormalMatrix[3]; // (World⁻¹)ᵀ 상단 3x3의 행 (w 미사용) — 비균등 스케일에서도 올바른 법선 변환
	uint32     BoneOffset = 0;
	uint32     Padding[3] = { 0, 0, 0 };
};
static_assert(sizeof(FInstanceGpuData) == 128);

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

// 파티클 카메라 상수 (Particle.hlsl b0)
struct alignas(16) FParticleFrameConstants
{
	FMatrix4x4 ViewProjection;
	FVector3   CameraRight;
	float      Padding0 = 0.0f;
	FVector3   CameraUp;
	float      Padding1 = 0.0f;
	FVector3   CameraPosition;
	float      Padding2 = 0.0f;
};
static_assert(sizeof(FParticleFrameConstants) == 112);

// 렌더러 하나의 그리기 상수 (Particle.hlsl b1)
struct alignas(16) FParticleDrawConstants
{
	FMatrix4x4 LocalToWorld;         // 로컬 공간 이미터면 이미터 월드 행렬, 아니면 단위 행렬
	int32      SubImageColumns = 1;
	int32      SubImageRows    = 1;
	int32      Alignment       = 0;  // EParticleSpriteAlignment
	int32      Padding0        = 0;
	float      VelocityStretch = 0.0f;
	float      Padding1[3]     = {};
};
static_assert(sizeof(FParticleDrawConstants) == 96);

// 입자 하나 (구조화 버퍼). CPU 이미터는 매 프레임 올리고, GPU 이미터는 계산 셰이더가 직접 쓴다.
//   ParticleCommon.hlsli의 FParticleData와 일치. Age >= Lifetime이면 죽은 입자
struct FParticleGpuData
{
	FVector3 Position;
	float    Age = 0.0f;
	FVector3 Velocity;
	float    Lifetime = 0.0f;
	FVector4 BaseColor;
	FVector4 Color;
	FVector2 BaseSize;
	FVector2 Size;
	float    Rotation   = 0.0f; // 도
	float    Mass       = 1.0f;
	float    SubImage   = 0.0f;
	uint32   SpawnIndex = 0;
};
static_assert(sizeof(FParticleGpuData) == 96);

// GPU 파티클 계산 상수 (ParticleSimulate.hlsl b0)
struct alignas(16) FParticleSimConstants
{
	FMatrix4x4 EmitterWorld;
	float      DeltaSeconds      = 0.0f;
	float      EmitterAlpha      = 0.0f;
	float      Time              = 0.0f;
	uint32     SpawnStart        = 0;
	uint32     SpawnCount        = 0;
	uint32     Capacity          = 0;
	uint32     Seed              = 0;
	uint32     bLocalSpace       = 0;
	uint32     SpawnModuleCount  = 0;
	uint32     SpawnOffset       = 0; // 프로그램 버퍼 안 float4 위치
	uint32     UpdateModuleCount = 0;
	uint32     UpdateOffset      = 0;
};
static_assert(sizeof(FParticleSimConstants) == 112);

// 리본 정점 (CPU가 만든다)
struct FParticleRibbonVertex
{
	FVector3 Position;
	FVector2 UV;
	FVector4 Color;
};
static_assert(sizeof(FParticleRibbonVertex) == 36);

// UI 프레임 상수 (UI.hlsl b0). 사각형 데이터는 FUIDrawQuad(UI/UIDrawList.h, 80바이트) = UI.hlsl FUIQuad
struct alignas(16) FUIConstants
{
	FVector2 ViewportSize;
	FVector2 Padding;
};
static_assert(sizeof(FUIConstants) == 16);

// UI 묶음 상수 (UI.hlsl b1)
struct alignas(16) FUIBatchConstants
{
	uint32 QuadOffset = 0;
	uint32 Padding[3] = {};
};
static_assert(sizeof(FUIBatchConstants) == 16);

// 점광원/스포트라이트 하나 (구조화 버퍼, Lighting.hlsli FLocalLight와 1:1). 식은 Renderer/LightMath.h
struct FLocalLightGpuData
{
	FVector3 Position;
	float    Radius = 0.0f;     // cm
	FVector3 Color;             // 선형 색 × 강도
	float    ConeScale  = 0.0f; // 점광원 0
	FVector3 Direction  = FVector3::ForwardVector;
	float    ConeOffset = 1.0f; // 점광원 1
	int32    ShadowIndex       = -1; // 그림자 타일 배열의 첫 장 (-1 = 그림자 없음, 점광원은 6장 연속)
	uint32   Type              = 0;  // LightMath::ELocalLightType
	float    ShadowTexelFactor = 0.0f; // 그림자 텍셀 월드 크기 = 깊이 × 이 값
	float    Padding0          = 0.0f;
};
static_assert(sizeof(FLocalLightGpuData) == 64);

// 클러스터드 라이팅 상수 (Mesh.hlsl b5, ClusterCulling.hlsl b0)
struct alignas(16) FClusterConstants
{
	FMatrix4x4 View;
	uint32     GridX      = 0;
	uint32     GridY      = 0;
	uint32     GridZ      = 0;
	uint32     LightCount = 0;
	FVector2   ScreenSize;          // 렌더 타깃 픽셀 크기 (픽셀 아트 모드는 저해상도)
	float      SliceScale = 0.0f;   // 조각 = floor(log(뷰 깊이) * SliceScale + SliceBias)
	float      SliceBias  = 0.0f;
	float      NearZ      = 0.0f;
	float      FarZ       = 0.0f;
	float      ProjScaleX = 0.0f;   // 원근: tan(반 시야각), 직교: 반 폭/높이
	float      ProjScaleY = 0.0f;
	uint32     bOrthographic      = 0;
	float      ShadowNormalOffset = 0.0f; // 그림자 텍셀 배수
	float      ShadowTexelSize    = 0.0f; // 1 / 그림자 타일 해상도
	uint32     Padding0           = 0;
};
static_assert(sizeof(FClusterConstants) == 128);

// 오클루전 컬링 항목 하나 = 메인 패스 정적 묶음의 인스턴스 하나 (OcclusionCulling.hlsl FOcclusionItem과 1:1)
struct FOcclusionItem
{
	FVector3 BoundsMin;
	uint32   Batch = 0;      // 메인 패스 묶음 번호 (간접 인자 = Batch * 2 + 단계)
	FVector3 BoundsMax;
	uint32   Instance = 0;   // 인스턴스 번호 (t13)
	uint32   BatchFirst = 0; // 묶음의 인스턴스 번호 목록 시작 위치 (단계별 출력 목록도 같은 구간)
	uint32   Padding[3] = {};
};
static_assert(sizeof(FOcclusionItem) == 48);

// 오클루전 컬링 상수 (OcclusionCulling.hlsl CullCS b0). 판정 식은 Renderer/HzbMath.h
struct alignas(16) FOcclusionCullConstants
{
	FMatrix4x4 ViewProjection;  // 1단계 = HZB를 만든 프레임의 뷰-투영, 2단계 = 현재
	FVector2   DepthSize;       // HZB를 만든 깊이 버퍼 크기 (픽셀)
	uint32     HzbMipCount = 0;
	uint32     ItemCount   = 0;
	uint32     Phase       = 1; // 1 = 이전 프레임 HZB로 검사, 2 = 1단계에서 가려진 항목을 이번 프레임 HZB로 다시 검사
	uint32     bHzbValid   = 0; // 0이면 모두 보이는 것으로 (첫 프레임, 크기 변경)
	uint32     Padding[2]  = {};
};
static_assert(sizeof(FOcclusionCullConstants) == 96);

// HZB 한 단계 만들기 상수 (OcclusionCulling.hlsl Hzb*CS 루트 상수 4개)
struct FHzbBuildConstants
{
	uint32 SourceWidth  = 0;
	uint32 SourceHeight = 0;
	uint32 DestWidth    = 0;
	uint32 DestHeight   = 0;
};
static_assert(sizeof(FHzbBuildConstants) == 16);

// 지형 상수 (Terrain.hlsl b0 space1, Phase 34). 높이 Z = Origin.Z + (16비트 높이) * HeightScale, 격자 XY = Origin.XY + 격자 * CellSize
struct alignas(16) FTerrainConstants
{
	FVector3 Origin;
	float    HeightScale = 0.0f; // cm / 16비트 단위
	FVector2 CellSize;
	float    Resolution  = 0.0f; // 정점 격자 한 변
	float    SkirtDepth  = 0.0f; // cm (LOD 단계 1당) — 청크 경계 균열 가리기
	FVector4 LayerTiling;        // 레이어별 1 / 타일 크기 (cm)
	FVector4 LayerBaseColor[4];  // 선형 (머티리얼 BaseColorFactor, 없으면 기본 지형 색)
	FVector4 LayerParams[4];     // 금속, 거칠기, 노멀 배율, AO 세기
	uint32   LayerCount = 4;
	uint32   DebugLod   = 0;     // 1이면 LOD별 색
	float    Padding[2] = {};
};
static_assert(sizeof(FTerrainConstants) == 192);

// 지형 청크 그리기 항목 (Terrain.hlsl FTerrainChunk, 구조화 버퍼 t2 space1). 같은 Quads끼리 인스턴싱
struct FTerrainChunkGpu
{
	uint32 X     = 0; // 시작 정점 (격자)
	uint32 Y     = 0;
	uint32 Step  = 1; // 정점 간격 (LOD 단계 = log2)
	uint32 Quads = 0; // 패치 한 변 사각형 수
};
static_assert(sizeof(FTerrainChunkGpu) == 16);

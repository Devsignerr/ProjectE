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
	uint32                     DecalsEnabled = 0; // 1이면 메인 패스가 DBuffer(t17~t19)를 섞는다
	FDirectionalLightConstants DirectionalLight;
	// 간이 환경광 (하늘/지면 반구). 이후 IBL이 대체한다
	FVector3                   SkyColor         = FVector3(0.35f, 0.45f, 0.6f);
	float                      AmbientIntensity = 1.0f;
	FVector3                   GroundColor      = FVector3(0.15f, 0.13f, 0.1f);
	float                      AmbientOcclusionEnabled = 0.0f; // 1이면 메인 패스가 SSAO(t16)를 간접광에 곱한다
	// 움직임 벡터 (지터 없음): 현재/이전 프레임 뷰-투영. 이력이 없으면 Prev = 현재
	FMatrix4x4                 UnjitteredViewProjection;
	FMatrix4x4                 PrevViewProjection;
	FVector2                   JitterNdc;        // 이번 프레임 투영 지터 (NDC)
	FVector2                   ScreenSize;       // 씬 타깃 픽셀 크기
	// 반사 (Phase 33-6): 캡처 목록(t20) 개수, SSR 결과(t22) 사용 여부, SSR 거칠기 한계
	uint32                     ReflectionCaptureCount = 0;
	uint32                     SsrEnabled             = 0;
	float                      SsrMaxRoughness        = 0.6f;
	float                      SsrIntensity           = 1.0f;
	// TAAU (Phase 48): 머티리얼/지형/폴리지 텍스처 샘플 밉 바이어스 (FUpscaleMath::ComputeMipBias, 네이티브 해상도면 0)
	float                      MaterialMipBias        = 0.0f;
	// 텍스처 밉 스트리밍 디버그 뷰 (r.DebugView mip): 1이면 메시 패스가 베이스 컬러 상주 밉을 색칠 (Mesh.hlsl MipDebugColor)
	uint32                     DebugMipView           = 0;
	// 레이 트레이싱 (Phase 50): 1 = 불투명 메인 패스의 방향광 그림자를 RT 마스크(t24)로 (반투명은 계속 섀도맵)
	uint32                     RayTracedShadows       = 0;
	float                      PerFramePadding        = 0.0f;
};
static_assert(sizeof(FPerFrameConstants) == 320);

// 반사 캡처 하나 (Mesh.hlsl t20 구조화 버퍼, FReflectionCaptureGpu와 1:1). Slot = 큐브 배열(t21) 안 큐브 번호
struct FReflectionCaptureGpuData
{
	FVector3 Position;
	uint32   Shape = 0; // 0 구, 1 상자
	FVector3 BoxExtent;
	float    Radius       = 0.0f;
	float    FadeDistance = 1.0f;
	float    Intensity    = 1.0f;
	uint32   Slot         = 0;
	float    Padding      = 0.0f;
};
static_assert(sizeof(FReflectionCaptureGpuData) == 48);

// 안개 적용 상수 (Fog.hlsli FogConstants와 1:1 — 전체 화면 적용 패스 b0, 파티클 b2). 밀도/감쇠/거리는 cm 단위
struct alignas(16) FFogConstants
{
	FVector3   Color;                     // 산란 색 (선형)
	float      Density       = 0.0f;      // 1/cm
	FVector3   DirectionalColor;
	float      HeightFalloff = 0.0f;      // 1/cm
	FVector3   LightDirection = FVector3(0.0f, 0.0f, -1.0f); // 빛 진행 방향
	float      BaseHeight    = 0.0f;
	FVector3   CameraPosition;
	float      StartDistance = 0.0f;
	FVector3   CameraForward = FVector3::ForwardVector;
	float      MaxOpacity    = 1.0f;
	float      DirectionalExponent      = 8.0f;
	float      DirectionalStartDistance = 0.0f;
	uint32     bEnabled      = 0;
	uint32     bVolumetric   = 0;
	float      VolumetricDistance = 0.0f;
	float      SkyDistance   = 100000.0f; // 기하 없는 픽셀(하늘)의 광선 길이
	FVector2   ScreenSize;
	FMatrix4x4 ViewProjection;            // 지터 없음 (볼륨 좌표)
	FMatrix4x4 InvViewProjection;         // 지터 포함 투영의 역 (깊이 → 월드)
	// 공중 원근 (Phase 49, 대기 컴포넌트 — FAtmosphereMath::FAerialParams, cm 단위). AerialEnabled 0이면 안개 결과 그대로
	FVector3   AerialRayleighScattering;  // 1/cm (지면 밀도)
	float      AerialRayleighScaleHeight = 800000.0f; // cm
	FVector3   AerialMieScattering;
	float      AerialMieScaleHeight = 120000.0f;
	FVector3   AerialMieExtinction;
	float      AerialMieAnisotropy = 0.8f;
	FVector3   AerialSunIlluminance;      // 카메라 고도 투과율 × 하늘 밝기 배율 포함
	uint32     AerialEnabled = 0;
	FVector3   AerialSunDirection = FVector3(0.0f, 0.0f, 1.0f);
	float      AerialGroundHeight = 0.0f; // cm (행성 표면 월드 Z)
	FVector3   AerialMultiScattering;     // Ψms × 태양 조도
	float      AerialDistanceScale = 1.0f;
};
static_assert(sizeof(FFogConstants) == 336);

// 볼류메트릭 안개 계산 상수 (VolumetricFog.hlsl b0)
struct alignas(16) FVolumetricFogConstants
{
	FMatrix4x4 InvViewProjection;   // 지터 없음 (격자 칸 → 월드 광선)
	FMatrix4x4 PrevViewProjection;  // 이전 프레임 (지터 없음, 시간 누적 재투영)
	FVector3   CameraPosition;
	float      VolumetricDistance = 6000.0f;
	FVector3   CameraForward = FVector3::ForwardVector;
	float      Density       = 0.0f; // 1/cm
	FVector3   LightDirection = FVector3(0.0f, 0.0f, -1.0f);
	float      HeightFalloff = 0.0f; // 1/cm
	FVector3   LightColor;           // 색 × 강도
	float      BaseHeight    = 0.0f;
	FVector3   Albedo        = FVector3::OneVector;
	float      ExtinctionScale = 1.0f;
	FVector3   AmbientColor;         // 고르게 들어오는 빛 (안개 색)
	float      Anisotropy    = 0.0f;
	uint32     GridX = 1;
	uint32     GridY = 1;
	uint32     GridZ = 1;
	float      SliceJitter   = 0.5f; // 조각 안 표본 위치 [0, 1)
	float      DirectionalScale = 1.0f;
	float      LocalLightScale  = 1.0f;
	float      HistoryWeight    = 0.9f;
	uint32     bHistoryValid    = 0;
};
static_assert(sizeof(FVolumetricFogConstants) == 256);

// 물리 기반 대기 (Phase 49, Atmosphere.hlsli AtmosphereConstants와 1:1 — LUT 계산·하늘 패스·하늘 큐브·구름 공용). 거리 km, 계수 1/km
//   좌표는 행성 중심 원점 Z-up (AtmosphereMath.h). SunIlluminance = 대기 위 태양 조도 × 하늘 밝기 배율 (하늘·공중 원근용)
struct alignas(16) FAtmosphereConstants
{
	FVector3 RayleighScattering;
	float    BottomRadius = 6360.0f;
	FVector3 MieScattering;
	float    TopRadius = 6460.0f;
	FVector3 MieExtinction;
	float    RayleighDensityExpScale = -1.0f / 8.0f; // 밀도 = exp(고도 × 이 값)
	FVector3 MieAbsorption;
	float    MieDensityExpScale = -1.0f / 1.2f;
	FVector3 OzoneAbsorption;
	float    MiePhaseG = 0.8f;
	FVector3 GroundAlbedo;
	float    OzoneCenterHeight = 25.0f;
	FVector3 SunDirection = FVector3(0.0f, 0.0f, 1.0f); // 태양 쪽 (정규화)
	float    OzoneHalfWidth = 15.0f;
	FVector3 SunIlluminance;
	float    SunDiskCosHalfAngle = 0.99999f;
	FVector3 CameraPosition;            // km (대기 좌표)
	float    SunDiskLuminance = 0.0f;   // 원반 휘도 = 조도 × 이 값 (= 배율 / 입체각)
	FVector3 NightSkyLuminance;         // × 하늘 밝기 배율
	float    StarIntensity = 0.0f;
	FVector3 SkyCameraForward = FVector3::ForwardVector; // 하늘 패스 시선 (지터 없음 — 하늘 상자와 같은 규약)
	float    SkyTanHalfFov = 1.0f;
	FVector3 SkyCameraRight = FVector3::RightVector;
	float    SkyAspect = 1.0f;
	FVector3 SkyCameraUp = FVector3::UpVector;
	uint32   bOrthographic = 0;
	FVector3 MoonDirection = FVector3(0.0f, 0.0f, -1.0f); // 달 쪽 (태양 반대)
	float    MoonDiskLuminance = 0.0f; // 달 원반 휘도 배율 (조도 = MoonIlluminance)
	FVector3 MoonIlluminance;           // 달빛 조도 × 색 (밤에만 0이 아님)
	float    AtmospherePadding0 = 0.0f;
};
static_assert(sizeof(FAtmosphereConstants) == 240);

// 볼류메트릭 구름 상수 (Phase 49, VolumetricClouds.hlsl CloudConstants b0). 거리 km (대기 좌표), 카메라 월드 위치만 cm
struct alignas(16) FCloudConstants
{
	FMatrix4x4 InvViewProjection;   // 지터 없음
	FMatrix4x4 PrevViewProjection;  // 지터 없음
	FMatrix4x4 ViewProjection;      // 지터 없음
	FVector3   CameraPositionWorld; // cm
	float      LayerBottomRadius = 6361.5f;
	FVector3   WindOffset;          // km
	float      LayerTopRadius = 6364.0f;
	float      Coverage   = 0.45f;
	float      CloudType  = 0.75f;
	float      Extinction = 40.0f;  // 1/km
	float      ShapeTile  = 18.0f;  // km
	float      DetailTile = 1.2f;
	float      WeatherTile = 40.0f;
	float      DetailStrength = 0.35f;
	float      SilverLining = 0.6f;
	FVector3   Albedo = FVector3::OneVector;
	float      AmbientScale = 1.0f;
	FVector3   LightDirection = FVector3(0.0f, 0.0f, 1.0f);
	float      CirrusCoverage = 0.0f;
	FVector3   LightIlluminance;
	float      CirrusRadius = 6368.0f;
	FVector2   TraceSize;
	FVector2   InvTraceSize;
	FVector2   Jitter = FVector2(0.5f, 0.5f);
	uint32     MaxSteps   = 48;
	uint32     FrameIndex = 0;
	float      HistoryWeight = 0.0f;
	uint32     bHistoryValid = 0;
	float      MaxDistance = 60.0f; // km
	float      CubeSize    = 32.0f;
};
static_assert(sizeof(FCloudConstants) == 352);

// 물 패스 프레임 상수 (Phase 49, Water.hlsl WaterFrame b0)
struct alignas(16) FWaterFrameConstants
{
	FMatrix4x4 ViewProjection;           // 지터 포함
	FMatrix4x4 UnjitteredViewProjection;
	FMatrix4x4 PrevViewProjection;
	FMatrix4x4 InvViewProjection;        // 지터 포함 투영의 역
	FVector3   CameraPosition;
	float      Time = 0.0f;
	FVector3   SunDirection = FVector3(0.0f, 0.0f, 1.0f); // 태양 쪽
	float      AmbientIntensity = 1.0f;
	FVector3   SunColor;                 // 색 × 강도 (대기 투과율 포함)
	uint32     ReflectionCaptureCount = 0;
	FVector2   ScreenSize;
	FVector2   InvScreenSize;
	uint32     bScreenReflections = 1;
	float      ReactiveMask = 0.2f;
	float      Padding[2] = {};
};
static_assert(sizeof(FWaterFrameConstants) == 336);

// 물 상자 하나 (Water.hlsl WaterBody b1). 식은 Renderer/WaterMath.h
struct alignas(16) FWaterBodyConstants
{
	FVector3 Center;
	float    CosYaw = 1.0f;
	FVector3 HalfSize;
	float    SinYaw = 0.0f;
	FVector3 ScatterColor;
	float    NormalStrength = 0.4f;
	FVector3 Absorption;               // 1/m
	float    WaveScale = 300.0f;       // cm
	FVector2 FlowDirection = FVector2(1.0f, 0.0f); // 월드 XY
	float    FlowSpeed = 0.0f;
	float    WaveSpeed = 15.0f;
	float    FoamIntensity = 0.6f;
	float    FoamDistance  = 25.0f;
	float    RefractionStrength = 0.04f;
	float    ReflectionIntensity = 1.0f;
	float    Roughness = 0.06f;
	float    Padding[3] = {};
};
static_assert(sizeof(FWaterBodyConstants) == 112);

// 메시 인스턴스 하나 (구조화 버퍼 t13, MeshInstance.hlsli FInstanceData와 1:1).
// 패스는 인스턴스 번호 목록(t14)의 [InstanceOffset, + 인스턴스 수) 구간을 DrawIndexedInstanced로 그린다
// 스킨 메시는 World/NormalMatrix 대신 BoneOffset(프레임 팔레트 버퍼 t15 안 첫 본 행렬 번호)을 쓴다
// 움직임 벡터: PrevWorld / PrevBoneOffset = 이전 프레임 값 (이력이 없으면 현재와 같다 → 물체 움직임 0)
struct FInstanceGpuData
{
	FMatrix4x4 World;
	FVector4   NormalMatrix[3]; // (World⁻¹)ᵀ 상단 3x3의 행 (w 미사용) — 비균등 스케일에서도 올바른 법선 변환
	uint32     BoneOffset     = 0;
	uint32     PrevBoneOffset = 0; // 스킨: 같은 팔레트 버퍼 안 이전 프레임 팔레트 첫 본
	uint32     Padding[2]     = { 0, 0 };
	FMatrix4x4 PrevWorld;
};
static_assert(sizeof(FInstanceGpuData) == 192);

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

// 그래프 머티리얼 상수 버퍼 머리 (MaterialCommon.hlsli / 생성 cbuffer MaterialGraphParameters의 MaterialHeader — b2).
// 뒤에 float4 MaterialParams[FMaterialParameterLayout::ConstantRegisters]가 이어진다 (MaterialRender::UploadMaterialConstants)
struct alignas(16) FMaterialGraphHeader
{
	float Time        = 0.0f; // 초 (E_MATERIAL_TIME)
	float AlphaCutoff = 0.5f; // Masked 컷오프 (E_MATERIAL_ALPHA_CUTOFF)
	float Padding0    = 0.0f;
	float Padding1    = 0.0f;
};
static_assert(sizeof(FMaterialGraphHeader) == 16);

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

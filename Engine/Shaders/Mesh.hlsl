#include "Common.hlsli"
#include "PBR.hlsli"
#include "SkinnedMesh.hlsli"
#include "Lighting.hlsli" // b5 클러스터 상수
#include "MeshInstance.hlsli" // t13/t14 인스턴스
#include "ScreenSpace.hlsli"
// 반투명 패스 안개 (FogRenderer 상수 b6 + 볼류메트릭 결과 t23, 선형 클램프 s3) — 불투명은 FogApply 전체 화면 패스
#define E_FOG_CONSTANTS_REGISTER b6
#define E_FOG_VOLUME_REGISTER t23
#define E_FOG_SAMPLER_REGISTER s3
#include "Fog.hlsli"

// 정적 메시 기본 셰이더: 금속/거칠기 PBR (glTF 2.0 텍스처 규약), 방향광 1개(캐스케이드 섀도우) + IBL
// + 점광원/스포트라이트(클러스터드: 픽셀의 클러스터 목록만 순회). 출력은 선형 HDR
// 머티리얼 블렌드 모드별 픽셀 셰이더 (Renderer/Material.h EMaterialBlendMode):
//   Opaque PSMain/PSPrepass, Masked PSMainMasked/PSPrepassMasked (베이스 알파 < AlphaCutoff 버림 — Shadow.hlsl ShadowMaskedPS와 같은 식),
//   Translucent PSTranslucent (알파 블렌드), Additive PSAdditive (가산). 양면 머티리얼은 뒷면(SV_IsFrontFace = false)에서 법선을 뒤집는다

struct FDirectionalLight
{
	float3 Direction; // 빛이 진행하는 방향 (정규화)
	float  Intensity;
	float3 Color;
	float  Padding0;
};

// 묶음 상수 (루트 상수): 인스턴스 번호 목록 안 시작 위치
cbuffer DrawConstants : register(b0)
{
	uint InstanceOffset;
};

cbuffer PerFrame : register(b1)
{
	float4x4          ViewProjection;
	float3            CameraPosition;
	uint              DecalsEnabled; // 1 = DBuffer(t17~t19) 사용
	FDirectionalLight DirectionalLight;
	float3            SkyColor;
	float             AmbientIntensity;
	float3            GroundColor;
	float             AmbientOcclusionEnabled; // 1 = SSAO(t16) 사용
	float4x4          UnjitteredViewProjection; // 움직임 벡터용 (지터 없음)
	float4x4          PrevViewProjection;       // 이전 프레임 (지터 없음)
	float2            JitterNdc;
	float2            ScreenSize;
	uint              ReflectionCaptureCount; // t20 개수
	uint              SsrEnabled;             // 1 = t22 사용
	float             SsrMaxRoughness;
	float             SsrIntensity;
	float             MaterialMipBias; // 머티리얼/지형 텍스처 밉 바이어스 (TAAU: log2(내부/출력), 네이티브 0)
	uint              DebugMipView;    // 1 = 텍스처 밉 스트리밍 디버그 뷰 (r.DebugView mip, MipDebugColor)
	uint              RayTracedShadows; // 1 = 불투명 방향광 그림자를 RT 마스크(t24)로 (Phase 50)
	float             PerFramePadding;
};

SamplerState LinearSampler : register(s0); // 이방성 반복 (머티리얼 E_MATERIAL_SAMPLER_WRAP)
SamplerState IblSampler    : register(s1); // 선형 클램프 (IBL, 머티리얼 E_MATERIAL_SAMPLER_CLAMP)

// 머티리얼 평가 (MaterialCommon.hlsli EvaluateMaterial): 고정 PBR(b2 Material + t0~t4) 또는 그래프 생성 코드(b2 + 공간 2 t0~)
// 래스터 메시 패스는 TAAU 밉 바이어스(PerFrame MaterialMipBias, 네이티브 0)를 모든 머티리얼 텍스처 샘플에 건다 → MATERIAL_SAMPLE = SampleBias
#define E_MATERIAL_MIP_BIAS MaterialMipBias
#include "MaterialCommon.hlsli"
#ifdef E_MATERIAL_GRAPH
#include "MaterialGraph.generated.hlsli"
#define E_MATERIAL_ALPHA_CUTOFF (E_MATERIAL_HEADER.y)
#else
#include "MaterialDefault.hlsli"
#endif

// 방향광 캐스케이드 섀도우 상수 b3 (ShadowCommon.hlsli — 볼류메트릭 안개와 공유)
#include "ShadowCommon.hlsli"

Texture2DArray<float>  ShadowMap     : register(t8);
SamplerComparisonState ShadowSampler : register(s2);

// 확산 맵은 irradiance / PI를 저장한다. 금속 반사에는 거칠기별 프리필터와 BRDF LUT를 사용한다.
TextureCube<float4> IblDiffuse : register(t5);
TextureCube<float4> IblSpecular : register(t6);
Texture2D<float2> IblBrdf : register(t7);

// 동적 GI — DDGI 프로브 볼륨 (Phase 51, DdgiCommon.hlsli): b9 상수, t40~t42 이번 프레임 아틀라스 (조도/거리/상태).
// DdgiVolumeCount = 0이고 디버그 뷰가 아니면 아래 IBL 식은 예전 그대로 (화면 비트 동일)
#define E_DDGI_CONSTANTS_REGISTER b9
#define E_DDGI_IRRADIANCE_REGISTER t40
#define E_DDGI_DISTANCE_REGISTER t41
#define E_DDGI_PROBE_DATA_REGISTER t42
#define E_DDGI_SAMPLER IblSampler
#include "DdgiCommon.hlsli"

// 점광원/스포트라이트 (LocalLightRenderer: 목록 + 클러스터별 인덱스)
StructuredBuffer<FLocalLight> LocalLights : register(t9);
StructuredBuffer<uint>        ClusterData : register(t10);
StructuredBuffer<float4x4>    LocalShadowMatrices : register(t11); // 그림자 장별 뷰-투영
Texture2DArray<float>         LocalShadowMap      : register(t12); // 그림자 타일 배열 (스포트·면광원 1장, 점광원 6장: +X,-X,+Y,-Y,+Z,-Z)

// 면광원 LTC 표·IES·쿠키 (Phase 52): 셰이더 가시 힙 전체 (루트 #26, 지형도 같은 공간 3) — 칸 번호는 라이트 목록/클러스터 상수
Texture2D LightTextures[] : register(t0, space3);
#define E_LIGHT_TEXTURE(Index) LightTextures[NonUniformResourceIndex(Index)]
#define E_LIGHT_SAMPLER_CLAMP IblSampler
#define E_LIGHT_SAMPLER_WRAP LinearSampler
#include "AreaLight.hlsli"

// 면광원 그림자 (스포트처럼 장 1장, 면 가운데에서 법선 쪽 원근): PCSS — 고정 Vogel 64탭 가림 탐색 → 반그림자 폭 = 면 반 크기 × (수신 - 가림) / 가림
// (AreaLightMath::ComputePenumbraUV, 결정적 — 픽셀마다 흔들지 않는다) → 같은 반경 64탭 비교 PCF. 폭은 1~32텍셀로 자른다
// 탭 수: 넓은 반그림자(최대 반경 32텍셀)를 16탭으로 덮으면 탭 사이가 텍셀 수십 개라 가림 평균 깊이와 PCF 결과가 텍셀을 넘을 때마다
// 계단으로 바뀌어 먼 벽·구석에 얼룩덜룩한 덩어리가 보였다 (2026-10-04 Tests/AreaLights — 256탭 기준 대비 구석 최대 오차 10.9 → 4.9 / 255, 720p 메인 패스 +0.4ms)
#define E_AREA_BLOCKER_TAPS 64
#define E_AREA_PCF_TAPS 64
float2 AreaShadowVogel(uint Index, uint Count)
{
	const float Radius = sqrt(((float)Index + 0.5f) / (float)Count);
	const float Angle  = (float)Index * 2.39996323f;
	return float2(cos(Angle), sin(Angle)) * Radius;
}

float ComputeAreaLightShadow(FLocalLight Light, float3 WorldPosition, float3 GeometricNormal, float3 L)
{
	const float3 FromLight = WorldPosition - Light.Position;
	const float  Depth     = dot(FromLight, Light.Direction);
	if (Depth <= 0.0f)
	{
		return 1.0f; // 양면 광원의 뒤쪽 (그림자 장은 앞쪽만)
	}
	const uint   Slice   = (uint)Light.ShadowIndex;
	const float  NdotL   = saturate(dot(GeometricNormal, L));
	const float  Texel   = max(Depth, 1.0f) * Light.ShadowTexelFactor;
	const float3 Offset  = GeometricNormal * Texel * LocalShadowNormalOffset * (1.0f - 0.5f * NdotL);
	const float4 ClipPos = mul(float4(WorldPosition + Offset, 1.0f), LocalShadowMatrices[Slice]);
	if (ClipPos.w <= 0.0f)
	{
		return 1.0f;
	}
	const float3 Ndc = ClipPos.xyz / ClipPos.w;
	const float2 UV  = Ndc.xy * float2(0.5f, -0.5f) + 0.5f;
	if (any(UV < 0.0f) || any(UV > 1.0f) || Ndc.z > 1.0f)
	{
		return 1.0f;
	}

	const float TwoTan   = Light.ShadowTexelFactor / max(LocalShadowTexelSize, 1.0e-6f); // 2 tan(반 시야각)
	const float Receiver = LinearizeLocalShadowDepth(Ndc.z, Light.ShadowFar);
	const float MinUV    = LocalShadowTexelSize;
	const float MaxUV    = LocalShadowTexelSize * 32.0f;
	const float Search   = clamp(Light.SourceRadius / max(TwoTan * Receiver, 1.0e-3f), MinUV, MaxUV); // 가림이 중간 깊이일 때의 반그림자
	const float Resolution = 1.0f / max(LocalShadowTexelSize, 1.0e-6f);
	// 수신 평면 기울기 바이어스: 넓은 커널이 기울어진 같은 면을 가림으로 읽지 않게 (커널 반경 월드 크기 × tan(빛 각) → 원근 깊이 차)
	const float CosL       = max(NdotL, 0.1f);
	const float TanL       = min(sqrt(1.0f - CosL * CosL) / CosL, 8.0f);
	const float Range      = Light.ShadowFar / max(Light.ShadowFar - LocalShadowNearZ, 1.0e-3f);
	const float DepthPerCm = LocalShadowNearZ * Range / max(Receiver * Receiver, 1.0e-3f);
	const float SlopePerUV = TwoTan * Receiver * TanL * DepthPerCm;

	float BlockerSum   = 0.0f;
	float BlockerCount = 0.0f;
	[unroll]
	for (uint Index = 0; Index < E_AREA_BLOCKER_TAPS; ++Index)
	{
		const float2 TapUV = saturate(UV + AreaShadowVogel(Index, E_AREA_BLOCKER_TAPS) * Search);
		const int2   Pixel = min(int2(TapUV * Resolution), int2(Resolution - 1.0f, Resolution - 1.0f));
		const float  Stored = LocalShadowMap.Load(int4(Pixel, Slice, 0));
		if (Stored < Ndc.z - Search * SlopePerUV)
		{
			BlockerSum += Stored;
			BlockerCount += 1.0f;
		}
	}
	if (BlockerCount <= 0.0f)
	{
		return 1.0f;
	}
	const float Blocker  = LinearizeLocalShadowDepth(BlockerSum / BlockerCount, Light.ShadowFar);
	const float Penumbra = clamp(Light.SourceRadius * max(Receiver - Blocker, 0.0f) / max(Blocker, 1.0f) / max(TwoTan * Receiver, 1.0e-3f), MinUV, MaxUV);
	float       Lit      = 0.0f;
	[unroll]
	for (uint Tap = 0; Tap < E_AREA_PCF_TAPS; ++Tap)
	{
		Lit += LocalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + AreaShadowVogel(Tap, E_AREA_PCF_TAPS) * Penumbra, Slice), Ndc.z - Penumbra * SlopePerUV);
	}
	return Lit / (float)E_AREA_PCF_TAPS;
}

// 1 = 빛 받음, 0 = 그림자. 3x3 PCF + 법선 오프셋 (텍셀 월드 크기 = 광원 기준 깊이 × ShadowTexelFactor)
float ComputeLocalShadow(FLocalLight Light, float3 WorldPosition, float3 GeometricNormal, float3 L)
{
	if (Light.ShadowIndex < 0)
	{
		return 1.0f;
	}
	const float3 FromLight = WorldPosition - Light.Position;
	uint         Slice     = (uint)Light.ShadowIndex;
	float        Depth;
	if (Light.Type == 0)
	{
		Slice += SelectCubeFace(FromLight);
		const float3 A = abs(FromLight);
		Depth          = max(A.x, max(A.y, A.z));
	}
	else
	{
		Depth = dot(FromLight, Light.Direction);
	}

	const float  NdotL   = saturate(dot(GeometricNormal, L));
	const float  Texel   = max(Depth, 1.0f) * Light.ShadowTexelFactor;
	const float3 Offset  = GeometricNormal * Texel * LocalShadowNormalOffset * (1.0f - 0.5f * NdotL);
	const float4 ClipPos = mul(float4(WorldPosition + Offset, 1.0f), LocalShadowMatrices[Slice]);
	if (ClipPos.w <= 0.0f)
	{
		return 1.0f;
	}
	const float3 Ndc = ClipPos.xyz / ClipPos.w;
	const float2 UV  = Ndc.xy * float2(0.5f, -0.5f) + 0.5f;
	if (any(UV < 0.0f) || any(UV > 1.0f) || Ndc.z > 1.0f)
	{
		return 1.0f;
	}

	float Lit = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			Lit += LocalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + float2(X, Y) * LocalShadowTexelSize, Slice), Ndc.z);
		}
	}
	return Lit / 9.0f;
}

uint GetClusterIndex(float2 PixelPosition, float3 WorldPosition)
{
	const uint  TileX     = min((uint)(PixelPosition.x / ClusterScreenSize.x * ClusterGridX), ClusterGridX - 1);
	const uint  TileY     = min((uint)(PixelPosition.y / ClusterScreenSize.y * ClusterGridY), ClusterGridY - 1);
	const float ViewDepth = mul(float4(WorldPosition, 1.0f), ClusterView).z;
	const uint  Slice     = ClusterDepthToSlice(ViewDepth, ClusterSliceScale, ClusterSliceBias, ClusterGridZ);
	return TileX + ClusterGridX * (TileY + ClusterGridY * Slice);
}

float3 EvaluateLocalLights(FSurface Surface, float2 PixelPosition, float3 WorldPosition, float3 GeometricNormal)
{
	if (LocalLightCount == 0)
	{
		return 0.0f;
	}
	const uint Base  = GetClusterIndex(PixelPosition, WorldPosition) * E_CLUSTER_STRIDE;
	const uint Count = ClusterData[Base];

	float3 Color = 0.0f;
	for (uint Index = 0; Index < Count; ++Index)
	{
		const FLocalLight Light    = LocalLights[ClusterData[Base + 1 + Index]];
		if (IsAreaLight(Light))
		{
			// 면광원 (Phase 52): LTC 다각형 적분 + IES/쿠키 + PCSS 그림자 (그림자 L = 가운데 방향)
			const float3 Area = EvaluateAreaLight(Light, Surface, WorldPosition);
			if (any(Area > 0.0f))
			{
				Color += Area * (Light.ShadowIndex < 0 ? 1.0f : ComputeAreaLightShadow(Light, WorldPosition, GeometricNormal, normalize(Light.Position - WorldPosition)));
			}
			continue;
		}
		const float3      ToLight  = Light.Position - WorldPosition;
		const float       Distance = length(ToLight);
		if (Distance >= Light.Radius)
		{
			continue;
		}
		const float3 L           = ToLight / max(Distance, 1.0e-4f);
		const float  Attenuation = LightDistanceAttenuation(Distance, Light.Radius) *
		                          LightConeAttenuation(dot(Light.Direction, -L), Light.ConeScale, Light.ConeOffset);
		if (Attenuation <= 0.0f)
		{
			continue;
		}
		float3 Radiance = Light.Color * Attenuation;
		if (Light.IesTexture >= 0 || Light.CookieTexture >= 0)
		{
			Radiance *= EvaluateLightProfile(Light, ToLightLocal(Light, -L)); // IES/쿠키 (Phase 52)
		}
		const float3 Direct = EvaluateDirectLight(Surface, L, Radiance);
		if (any(Direct > 0.0f))
		{
			Color += Direct * ComputeLocalShadow(Light, WorldPosition, GeometricNormal, L);
		}
	}
	return Color;
}

// 반사 캡처 (ReflectionCaptures.h, 식은 Renderer/ReflectionMath.h): 목록은 우선순위 순, 앞에서부터 남은 비중을 채운다
struct FReflectionCaptureGpu
{
	float3 Position;
	uint   Shape; // 0 구, 1 상자 (월드 축 정렬, 시차 보정)
	float3 BoxExtent;
	float  Radius;
	float  FadeDistance;
	float  Intensity;
	uint   Slot;
	float  Padding;
};
StructuredBuffer<FReflectionCaptureGpu> ReflectionCaptures       : register(t20);
TextureCubeArray<float4>                ReflectionCaptureAtlas   : register(t21); // 프리필터 밉 = 하늘 IBL과 같은 거칠기 대응
Texture2D<float4>                       ScreenSpaceReflection    : register(t22); // rgb 색, a 신뢰도 (SsrTrace.hlsl)

float ComputeCaptureInfluence(FReflectionCaptureGpu Capture, float3 P)
{
	if (Capture.Shape == 0)
	{
		return saturate((Capture.Radius - distance(P, Capture.Position)) / Capture.FadeDistance);
	}
	const float3 D = Capture.BoxExtent - abs(P - Capture.Position);
	return saturate(min(D.x, min(D.y, D.z)) / Capture.FadeDistance);
}

float3 ParallaxCorrect(FReflectionCaptureGpu Capture, float3 P, float3 R)
{
	const float3 BoxMin = Capture.Position - Capture.BoxExtent;
	const float3 BoxMax = Capture.Position + Capture.BoxExtent;
	const float3 Planes = select(R > 0.0f, BoxMax, BoxMin);
	const float3 T      = select(abs(R) > 1.0e-6f, (Planes - P) / R, 1.0e30f);
	const float  TExit  = max(min(T.x, min(T.y, T.z)), 0.0f);
	return normalize(P + R * TExit - Capture.Position);
}

// 반사 광원: SSR(신뢰도 × 거칠기 페이드) → 캡처 → 하늘 프리필터 (AmbientIntensity는 하늘에만 — 캡처/SSR은 장면 밝기 그대로)
// bScreenReflections = false: SSR 결과를 쓰지 않는다 (반투명 — SSR 표는 불투명 표면 기준)
float3 SampleSpecularEnvironment(float3 R, float Roughness, float3 WorldPosition, float2 PixelPosition, float MipCount, bool bScreenReflections)
{
	const float Lod       = Roughness * (MipCount - 1);
	float3      Color     = 0.0f;
	float       Remaining = 1.0f;
	for (uint Index = 0; Index < ReflectionCaptureCount && Remaining > 0.01f; ++Index)
	{
		const FReflectionCaptureGpu Capture = ReflectionCaptures[Index];
		const float                 Weight  = ComputeCaptureInfluence(Capture, WorldPosition);
		if (Weight <= 0.0f)
		{
			continue;
		}
		const float3 Dir = Capture.Shape == 1 ? ParallaxCorrect(Capture, WorldPosition, R) : R;
		Color += ReflectionCaptureAtlas.SampleLevel(IblSampler, float4(Dir, (float)Capture.Slot), Lod).rgb * (Capture.Intensity * Weight * Remaining);
		Remaining *= 1.0f - Weight;
	}
	Color += IblSpecular.SampleLevel(IblSampler, R, Lod).rgb * (AmbientIntensity * Remaining);

	if (bScreenReflections && SsrEnabled != 0)
	{
		const float4 Ssr  = ScreenSpaceReflection.Load(int3(PixelPosition, 0));
		const float  Fade = saturate((SsrMaxRoughness - Roughness) / max(SsrMaxRoughness * 0.5f, 1.0e-3f)); // ReflectionMath::ComputeSsrRoughnessFade
		Color             = lerp(Color, Ssr.rgb * SsrIntensity, saturate(Ssr.a) * Fade);
	}
	return Color;
}

float3 EvaluateImageBasedLightingEx(FSurface Surface, float3 WorldPosition, float2 PixelPosition, bool bScreenReflections)
{
	const float NdotV = max(saturate(dot(Surface.N, Surface.V)), 1.0e-4f);
	const float3 F0 = GetF0(Surface);
	const float3 F = F0 + (max(1.0f - Surface.Roughness, F0) - F0) * pow(1.0f - NdotV, 5.0f);
	const float3 Diffuse = IblDiffuse.SampleLevel(IblSampler, Surface.N, 0).rgb * Surface.Albedo;
	uint Width, Height, MipCount;
	IblSpecular.GetDimensions(0, Width, Height, MipCount);
	const float3 R = reflect(-Surface.V, Surface.N);
	const float3 Prefiltered = SampleSpecularEnvironment(R, Surface.Roughness, WorldPosition, PixelPosition, (float)MipCount, bScreenReflections);
	const float2 Brdf = IblBrdf.SampleLevel(IblSampler, float2(NdotV, Surface.Roughness), 0);
	const float3 Specular = Prefiltered * (F0 * Brdf.x + Brdf.y);
	if (DdgiVolumeCount != 0 || DdgiDebugView != 0)
	{
		// 동적 GI (Phase 51): 볼륨 안은 프로브 조도(삼선형 + 체비셰프 가시성), 경계 페이드·볼륨 밖은 남은 비중만 하늘 IBL 조도. 반사는 그대로
		float        Remaining    = 1.0f;
		const float3 Probes       = DdgiVolumeCount != 0 ? EvaluateDdgiIrradiance(WorldPosition, Surface.N, Surface.V, Remaining) : 0.0f;
		const float3 DiffuseLight = (1.0f - F) * (1.0f - Surface.Metallic) * Surface.Albedo *
		                            (Probes + IblDiffuse.SampleLevel(IblSampler, Surface.N, 0).rgb * (AmbientIntensity * Remaining));
		if (DdgiDebugView != 0)
		{
			return DiffuseLight * Surface.Occlusion; // --debug-view gi: 간접 확산만
		}
		return (DiffuseLight + Specular) * Surface.Occlusion;
	}
	return ((1.0f - F) * (1.0f - Surface.Metallic) * Diffuse * AmbientIntensity + Specular) * Surface.Occlusion;
}

float3 EvaluateImageBasedLighting(FSurface Surface, float3 WorldPosition, float2 PixelPosition)
{
	return EvaluateImageBasedLightingEx(Surface, WorldPosition, PixelPosition, true);
}

uint SelectCascade(float3 WorldPosition)
{
	const float ViewDepth = dot(WorldPosition - CameraPosition, ShadowCameraForward);
	[unroll]
	for (uint Index = 0; Index < 4; ++Index)
	{
		if (Index < CascadeCount && ViewDepth <= CascadeSplits[Index])
		{
			return Index;
		}
	}
	return CascadeCount; // 그림자 거리 밖
}

// 1 = 완전히 빛 받음, 0 = 그림자. 3x3 PCF + 법선 오프셋, 마지막 캐스케이드 끝에서 페이드
float ComputeShadow(float3 WorldPosition, float3 GeometricNormal, float3 L)
{
	if (ShadowEnabled < 0.5f)
	{
		return 1.0f;
	}
	const uint Cascade = SelectCascade(WorldPosition);
	if (Cascade >= CascadeCount)
	{
		return 1.0f;
	}

	// 빛에 비스듬한 면일수록 더 밀어 자기 그림자(acne)를 줄인다
	const float  NdotL    = saturate(dot(GeometricNormal, L));
	const float3 Offset   = GeometricNormal * CascadeTexelWorld[Cascade] * ShadowNormalOffset * (1.0f - 0.5f * NdotL);
	const float4 ClipPos  = mul(float4(WorldPosition + Offset, 1.0f), CascadeViewProjection[Cascade]);
	const float2 UV       = ClipPos.xy * float2(0.5f, -0.5f) + 0.5f;
	const float  Depth    = ClipPos.z;
	if (any(UV < 0.0f) || any(UV > 1.0f) || Depth > 1.0f)
	{
		return 1.0f;
	}

	float Lit = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			Lit += ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + float2(X, Y) * ShadowTexelSize, Cascade), Depth);
		}
	}
	Lit /= 9.0f;

	// 그림자 거리 끝 10%에서 부드럽게 사라지게
	const float ViewDepth = dot(WorldPosition - CameraPosition, ShadowCameraForward);
	const float FadeStart = CascadeSplits[CascadeCount - 1] * 0.9f;
	const float Fade      = saturate((ViewDepth - FadeStart) / max(CascadeSplits[CascadeCount - 1] - FadeStart, 1.0e-3f));
	return lerp(Lit, 1.0f, Fade);
}

// 레이 트레이싱 방향광 그림자 마스크 (Phase 50, RayTracedShadows.hlsl — 사전 패스 깊이 기준 화면 픽셀 값). 불투명 표면만:
// 반투명(bScreenEffects = false)·볼류메트릭 안개는 섀도맵. 꺼져 있으면 ComputeShadow 그대로 (화면 비트 동일)
Texture2D<float> RayTracedShadowMask : register(t24);

float ComputeDirectionalShadow(float3 WorldPosition, float3 GeometricNormal, float3 L, float2 PixelPosition, bool bScreenEffects)
{
	if (bScreenEffects && RayTracedShadows != 0)
	{
		return RayTracedShadowMask.Load(int3(PixelPosition, 0));
	}
	return ComputeShadow(WorldPosition, GeometricNormal, L);
}

float3 CascadeDebugColor(float3 WorldPosition)
{
	const uint Cascade = SelectCascade(WorldPosition);
	const float3 Colors[5] = { float3(1.0f, 0.3f, 0.3f), float3(0.3f, 1.0f, 0.3f), float3(0.3f, 0.3f, 1.0f),
	                           float3(1.0f, 1.0f, 0.3f), float3(1.0f, 1.0f, 1.0f) };
	return Colors[min(Cascade, 4u)];
}

struct FVertexInput
{
	float3 Position : POSITION;
	float3 Normal   : NORMAL;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
	float4 Tangent  : TANGENT; // xyz = +U, w = 바이탄젠트 부호 (B = cross(N, T) * w = 텍스처 위쪽)
};

struct FPixelInput
{
	float4 Position      : SV_Position;
	float3 WorldPosition : POSITION0;
	float3 WorldNormal   : NORMAL;
	float4 WorldTangent  : TANGENT;
	float2 UV            : TEXCOORD0;
	float4 Color         : COLOR;
	// 움직임 벡터 (지터 없는 현재/이전 클립 좌표). 깊이 사전 패스와 메인 패스가 같은 정점 셰이더를 써야 깊이 같음 테스트가 맞는다
	float4 CurrentClip   : TEXCOORD1;
	float4 PreviousClip  : TEXCOORD2;
};

FPixelInput VSMain(FVertexInput Input, uint InstanceId : SV_InstanceID)
{
	FPixelInput Output;

	const FInstanceData Instance      = LoadInstance(InstanceOffset, InstanceId);
	const float4        WorldPosition = mul(float4(Input.Position, 1.0f), Instance.World);
	Output.Position      = mul(WorldPosition, ViewProjection);
	Output.CurrentClip   = mul(WorldPosition, UnjitteredViewProjection);
	Output.PreviousClip  = mul(mul(float4(Input.Position, 1.0f), Instance.PrevWorld), PrevViewProjection);
	Output.WorldPosition = WorldPosition.xyz;
	Output.WorldNormal   = normalize(mul(Input.Normal, GetNormalMatrix(Instance)));

	// 탄젠트는 표면을 따라가는 벡터이므로 World로 변환. 반사(음수 스케일)면 바이탄젠트 부호도 뒤집는다
	const float3x3 World3     = (float3x3)Instance.World;
	const float    Handedness = determinant(World3) < 0.0f ? -1.0f : 1.0f;
	Output.WorldTangent = float4(normalize(mul(Input.Tangent.xyz, World3)), Input.Tangent.w * Handedness);
	Output.UV           = Input.UV;
	Output.Color        = Input.Color;
	return Output;
}

#ifdef E_SKIN_CACHE
// 스킨 메시 (스킨 캐시, Renderer/SkinCache.h): 계산 셰이더가 이번 프레임 한 번 스키닝한 월드 공간 정점을 읽는다 (슬롯 1 스트림·팔레트 없음).
// 값은 아래 팔레트 경로와 같은 식의 결과 — 사전 패스와 메인 패스가 같은 정점 셰이더라 깊이 EQUAL이 맞는다
FPixelInput VSSkinned(FVertexInput Input, uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	FPixelInput Output;

	const FInstanceData    Instance = LoadInstance(InstanceOffset, InstanceId);
	const FSkinCacheVertex Vertex   = LoadSkinCacheVertex(Instance.SkinCacheVertex, Instance.SkinCacheCapacity, VertexId);
	Output.Position      = mul(Vertex.Position, ViewProjection);
	Output.CurrentClip   = mul(Vertex.Position, UnjitteredViewProjection);
	Output.PreviousClip  = mul(Vertex.PrevPosition, PrevViewProjection);
	Output.WorldPosition = Vertex.Position.xyz;
	Output.WorldNormal   = Vertex.Normal;
	Output.WorldTangent  = Vertex.Tangent;
	Output.UV            = Input.UV;
	Output.Color         = Input.Color;
	return Output;
}
#else
struct FSkinnedVertexInput
{
	float3 Position : POSITION;
	float3 Normal   : NORMAL;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
	float4 Tangent  : TANGENT;
	uint4  Joints   : BLENDINDICES; // 슬롯 1 스킨 스트림
	float4 Weights  : BLENDWEIGHT;
};

// 스킨 메시 (인스턴싱): 인스턴스의 BoneOffset 팔레트로 바로 월드 공간 (인스턴스 행렬 없음).
// 본 행렬은 균등 스케일 + 회전 + 이동을 가정해 법선도 같은 3x3으로 변환
FPixelInput VSSkinned(FSkinnedVertexInput Input, uint InstanceId : SV_InstanceID)
{
	FPixelInput Output;

	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	const float4x4      Skin     = ComputeSkinMatrix(Instance.BoneOffset, Input.Joints, Input.Weights);
	const float4   WorldPosition = mul(float4(Input.Position, 1.0f), Skin);
	const float3x3 Skin3         = (float3x3)Skin;
	Output.Position      = mul(WorldPosition, ViewProjection);
	Output.CurrentClip   = mul(WorldPosition, UnjitteredViewProjection);
	const float4x4 PrevSkin = ComputeSkinMatrix(Instance.PrevBoneOffset, Input.Joints, Input.Weights);
	Output.PreviousClip  = mul(mul(float4(Input.Position, 1.0f), PrevSkin), PrevViewProjection);
	Output.WorldPosition = WorldPosition.xyz;
	Output.WorldNormal   = normalize(mul(Input.Normal, Skin3));

	const float Handedness = determinant(Skin3) < 0.0f ? -1.0f : 1.0f;
	Output.WorldTangent = float4(normalize(mul(Input.Tangent.xyz, Skin3)), Input.Tangent.w * Handedness);
	Output.UV           = Input.UV;
	Output.Color        = Input.Color;
	return Output;
}
#endif

// SSAO (반해상도 R = 가시도, G = 뷰 깊이, AmbientOcclusion.hlsl): 4탭 깊이 가중 업샘플. 간접광에만 곱한다
Texture2D<float2> ScreenAmbientOcclusion : register(t16);

float SampleScreenAmbientOcclusion(float2 PixelPosition, float3 WorldPosition)
{
	if (AmbientOcclusionEnabled < 0.5f)
	{
		return 1.0f;
	}
	uint Width, Height;
	ScreenAmbientOcclusion.GetDimensions(Width, Height);
	const float  ViewDepth = mul(float4(WorldPosition, 1.0f), ClusterView).z;
	// 반해상도 픽셀 i는 전체 해상도 픽셀 2i에서 계산됐다 → 전체 위치 x의 반해상도 좌표 = (x - 0.5) / 2
	// 픽셀 아트는 전체 해상도(버퍼 폭 = 씬 폭)로 계산하므로 나눗셈 1 → 자기 픽셀 그대로
	const float  Divisor = (float)Width * 2.0f > ScreenSize.x + 1.5f ? 1.0f : 2.0f;
	const float2 HalfPos = (PixelPosition - 0.5f) / Divisor;
	const int2   Base    = int2(floor(HalfPos));
	const float2 F       = HalfPos - float2(Base);
	const int2   MaxPixel = int2(Width, Height) - 1;

	float Sum    = 0.0f;
	float Weight = 0.0f;
	float Nearest = 1.0f;
	float NearestDelta = 1.0e30f;
	[unroll]
	for (int Tap = 0; Tap < 4; ++Tap)
	{
		const int2   Offset = int2(Tap & 1, Tap >> 1);
		const float2 Sample = ScreenAmbientOcclusion.Load(int3(clamp(Base + Offset, int2(0, 0), MaxPixel), 0));
		const float  Bilinear = (Offset.x == 1 ? F.x : 1.0f - F.x) * (Offset.y == 1 ? F.y : 1.0f - F.y);
		const float  Delta    = abs(Sample.y - ViewDepth) / max(ViewDepth, 1.0e-3f);
		const float  W        = Bilinear * exp(-Delta * 40.0f) + 1.0e-5f;
		Sum += Sample.x * W;
		Weight += W;
		if (Delta < NearestDelta)
		{
			NearestDelta = Delta;
			Nearest      = Sample.x;
		}
	}
	return Weight > 1.0e-3f ? Sum / Weight : Nearest;
}

// 데칼 DBuffer (DecalRenderer/Decal.hlsl, 식은 Renderer/DecalMath.h): rgb = 값·불투명도 누적, a = 남은 원래 표면 비중
Texture2D<float4> DBufferA : register(t17); // 베이스색 (sRGB → 선형으로 읽힘)
Texture2D<float4> DBufferB : register(t18); // 월드 법선 * 0.5 + 0.5
Texture2D<float4> DBufferC : register(t19); // R 거칠기, G 금속

void ApplyDecals(float2 PixelPosition, inout FSurface Surface)
{
	const int3   Pixel = int3(PixelPosition, 0);
	const float4 A     = DBufferA.Load(Pixel);
	const float4 B     = DBufferB.Load(Pixel);
	const float4 C     = DBufferC.Load(Pixel);
	Surface.Albedo     = Surface.Albedo * A.a + A.rgb;
	Surface.N          = normalize(Surface.N * B.a + B.rgb * 2.0f - (1.0f - B.a));
	Surface.Roughness  = Surface.Roughness * C.a + C.r;
	Surface.Metallic   = saturate(Surface.Metallic * C.a + C.g);
}

// 머티리얼 표면 (EvaluateMaterial 결과를 월드 공간으로) + 기하 법선 + 발광 + 알파
struct FMeshSurface
{
	FSurface Surface;
	float3   GeometricNormal;
	float3   Emissive;
	float    Alpha;
};

// 메시 정점 보간값 → 머티리얼 입력 (DXR 히트 셰이더는 무게중심 보간으로 같은 값을 채운다 — MaterialCommon.hlsli)
FMaterialPixelInputs MakeMaterialInputs(FPixelInput Input, bool bFrontFace)
{
	FMaterialPixelInputs In;
	In.WorldPosition = Input.WorldPosition;
	In.WorldNormal   = normalize(Input.WorldNormal);
	In.WorldTangent  = Input.WorldTangent;
	In.UV0           = Input.UV;
	In.VertexColor   = Input.Color;
	In.CameraVector  = normalize(CameraPosition - Input.WorldPosition);
	In.PixelPosition = Input.Position.xy;
	In.bFrontFace    = bFrontFace;
	return In;
}

FMaterialSurface EvaluateMeshMaterial(FPixelInput Input, bool bFrontFace)
{
	FMaterialSurface Material;
	EvaluateMaterial(MakeMaterialInputs(Input, bFrontFace), Material);
	return Material;
}

// bScreenEffects: 불투명 표면 기준 화면 버퍼(데칼 DBuffer, SSAO)를 쓴다 (반투명은 false)
FMeshSurface MakeMeshSurface(FPixelInput Input, bool bFrontFace, bool bScreenEffects, FMaterialSurface Material)
{
	FMeshSurface Result;
	Result.Emissive         = Material.Emissive;
	Result.Alpha            = Material.Opacity;
	Result.GeometricNormal  = normalize(Input.WorldNormal);
	Result.Surface.Albedo    = Material.BaseColor;
	Result.Surface.Metallic  = saturate(Material.Metallic);
	Result.Surface.Roughness = Material.Roughness;
	Result.Surface.N         = MaterialTangentToWorld(normalize(Input.WorldNormal), Input.WorldTangent, Material.Normal);
	if (!bFrontFace)
	{
		// 양면 머티리얼의 뒷면 (한 면 머티리얼은 뒷면을 컬링하므로 여기 오지 않는다): 표면 반대쪽에서 보므로 법선을 뒤집는다
		Result.Surface.N       = -Result.Surface.N;
		Result.GeometricNormal = -Result.GeometricNormal;
	}
	if (bScreenEffects && DecalsEnabled != 0)
	{
		ApplyDecals(Input.Position.xy, Result.Surface);
	}
	Result.Surface.Roughness = clamp(Result.Surface.Roughness, 0.045f, 1.0f); // 너무 작은 거칠기는 하이라이트 에일리어싱
	Result.Surface.V         = normalize(CameraPosition - Input.WorldPosition);
	Result.Surface.Occlusion = Material.AmbientOcclusion; // IBL만 사용
	if (bScreenEffects)
	{
		Result.Surface.Occlusion *= SampleScreenAmbientOcclusion(Input.Position.xy, Input.WorldPosition);
	}
	return Result;
}

// 방향광(그림자) + 로컬 라이트 + IBL. bScreenReflections = false면 SSR 없이 캡처/하늘만
float3 EvaluateMeshLighting(FSurface Surface, float3 WorldPosition, float3 GeometricNormal, float2 PixelPosition, bool bScreenReflections)
{
	const float3 L        = -DirectionalLight.Direction; // 표면 → 광원
	float3       Radiance = DirectionalLight.Color * DirectionalLight.Intensity;
	if (DirectionalCookieTexture >= 0)
	{
		Radiance *= EvaluateDirectionalCookie(WorldPosition, MaterialMipBias); // 방향광 쿠키 (Phase 52)
	}
	const float  Shadow   = ComputeDirectionalShadow(WorldPosition, GeometricNormal, L, PixelPosition, bScreenReflections);

	float3 Color = EvaluateDirectLight(Surface, L, Radiance) * Shadow;
	Color += EvaluateLocalLights(Surface, PixelPosition, WorldPosition, GeometricNormal);
	Color = DdgiDebugView != 0 ? 0.0f : Color; // --debug-view gi: 직접광 없이 간접 확산만
	Color += EvaluateImageBasedLightingEx(Surface, WorldPosition, PixelPosition, bScreenReflections);
	return Color;
}

// 텍스처 밉 스트리밍 디버그 뷰 (r.DebugView mip, PerFrame DebugMipView): 고정 PBR 베이스 컬러 텍스처가 이 픽셀에서 원하는 LOD를
// 상주 텍스처 기준(밉 0 = 상주 최상위 밉)으로 본다. LOD < 0 = 상주 범위보다 세밀한 밉이 필요 (주황 → 빨강, 부족한 밉 수만큼),
// 0~2 = 알맞음 (초록 — 스트리밍 여유 1밉 포함), 2 이상 = 여유 (하늘 → 파랑). 텍스처 없음(1x1 기본)·그래프 머티리얼은 회색. 밝기는 알베도
float3 MipDebugColor(float2 UV, float3 Albedo)
{
	const float Shade = 0.35f + 0.65f * saturate(dot(Albedo, float3(0.3f, 0.59f, 0.11f)));
#ifdef E_MATERIAL_GRAPH
	return 0.3f * Shade;
#else
	uint Width, Height, Levels;
	BaseColorTexture.GetDimensions(0, Width, Height, Levels);
	if (Width <= 1 && Height <= 1)
	{
		return 0.3f * Shade;
	}
	const float Lod = BaseColorTexture.CalculateLevelOfDetailUnclamped(LinearSampler, UV) + MaterialMipBias;
	float3      Color;
	if (Lod < 0.0f)
	{
		Color = lerp(float3(1.0f, 0.6f, 0.0f), float3(1.0f, 0.0f, 0.0f), saturate(-Lod));
	}
	else if (Lod < 2.0f)
	{
		Color = float3(0.1f, 1.0f, 0.1f);
	}
	else
	{
		Color = lerp(float3(0.1f, 0.6f, 1.0f), float3(0.0f, 0.0f, 1.0f), saturate((Lod - 2.0f) * 0.5f));
	}
	return Color * Shade;
#endif
}

float4 ShadeOpaque(FPixelInput Input, FMeshSurface Mesh)
{
	float3 Color = EvaluateMeshLighting(Mesh.Surface, Input.WorldPosition, Mesh.GeometricNormal, Input.Position.xy, true) +
	               (DdgiDebugView != 0 ? 0.0f : Mesh.Emissive);
	if (VisualizeCascades != 0)
	{
		Color *= CascadeDebugColor(Input.WorldPosition);
	}
	if (DebugMipView != 0)
	{
		Color = MipDebugColor(Input.UV, Mesh.Surface.Albedo) * 2.0f;
	}
	return float4(Color, 0.0f); // 알파 = TAA 반응형 마스크 (불투명 0, 파티클/반투명이 덮은 만큼 쌓인다)
}

float4 PSMain(FPixelInput Input, bool bFrontFace : SV_IsFrontFace) : SV_Target
{
	return ShadeOpaque(Input, MakeMeshSurface(Input, bFrontFace, true, EvaluateMeshMaterial(Input, bFrontFace)));
}

float4 PSMainMasked(FPixelInput Input, bool bFrontFace : SV_IsFrontFace) : SV_Target
{
	const FMaterialSurface Material = EvaluateMeshMaterial(Input, bFrontFace);
	clip(Material.OpacityMask - E_MATERIAL_ALPHA_CUTOFF);
	return ShadeOpaque(Input, MakeMeshSurface(Input, bFrontFace, true, Material));
}

// 반투명 (알파 블렌드 Src·SrcA + Dst·(1 - SrcA), 깊이 쓰기 없음): 확산은 알파만큼, 반사(스펙큘러)는 알파와 무관하게 더한다.
//   반사 성분 = 같은 F0의 금속 표면(확산 0)으로 한 번 더 조명 → 프리멀티플라이드 색 P = Spec + (Full - Spec)·a + 발광·a
//   배경 투과 = (1 - a)(1 - 반사율 평균) → 출력 알파(덮인 정도 = TAA 반응형 마스크) C = 1 - 투과, 색 = P / C
//   안개는 파티클과 같이 직접: 색 × 투과율 + 산란 (Fog.hlsli EvaluateFog). 화면 버퍼(SSAO/데칼/SSR)는 쓰지 않는다
float4 PSTranslucent(FPixelInput Input, bool bFrontFace : SV_IsFrontFace) : SV_Target
{
	const FMeshSurface Mesh  = MakeMeshSurface(Input, bFrontFace, false, EvaluateMeshMaterial(Input, bFrontFace));
	const float        Alpha = saturate(Mesh.Alpha);

	FSurface Reflective  = Mesh.Surface;
	Reflective.Albedo    = GetF0(Mesh.Surface);
	Reflective.Metallic  = 1.0f;
	const float3 Full    = EvaluateMeshLighting(Mesh.Surface, Input.WorldPosition, Mesh.GeometricNormal, Input.Position.xy, false);
	const float3 Specular = EvaluateMeshLighting(Reflective, Input.WorldPosition, Mesh.GeometricNormal, Input.Position.xy, false);
	const float3 Premultiplied = Specular + (Full - Specular) * Alpha + Mesh.Emissive * Alpha;

	const float  NdotV       = max(saturate(dot(Mesh.Surface.N, Mesh.Surface.V)), 1.0e-4f);
	const float2 Brdf        = IblBrdf.SampleLevel(IblSampler, float2(NdotV, Mesh.Surface.Roughness), 0);
	const float3 Reflectance = GetF0(Mesh.Surface) * Brdf.x + Brdf.y;
	const float  Reflect     = saturate((Reflectance.r + Reflectance.g + Reflectance.b) / 3.0f);
	const float  Coverage    = max(1.0f - (1.0f - Alpha) * (1.0f - Reflect), 1.0e-3f);

	const float4 Fog = EvaluateFog(Input.WorldPosition);
	return float4(Premultiplied * Fog.a / Coverage + Fog.rgb, Coverage);
}

// 가산 (Src + Dst, 깊이 쓰기 없음): 조명된 색 × 알파를 더한다. 알파 = 덮인 정도 (TAA 반응형 마스크 — 파티클 PSAdditive와 같음)
// 안개: 빛을 더하므로 투과율만 곱한다
float4 PSAdditive(FPixelInput Input, bool bFrontFace : SV_IsFrontFace) : SV_Target
{
	const FMeshSurface Mesh  = MakeMeshSurface(Input, bFrontFace, false, EvaluateMeshMaterial(Input, bFrontFace));
	const float        Alpha = saturate(Mesh.Alpha);
	const float3       Color = EvaluateMeshLighting(Mesh.Surface, Input.WorldPosition, Mesh.GeometricNormal, Input.Position.xy, false) + Mesh.Emissive;
	return float4(Color * Alpha * EvaluateFog(Input.WorldPosition).a, Alpha);
}
// 깊이 사전 패스 (FSceneRenderer): 깊이 + 화면 공간 법선(기하 법선, 팔면체) + 거칠기(머티리얼 Roughness) + 움직임 벡터
struct FPrepassOutput
{
	float4 Normal   : SV_Target0; // R10G10B10A2_UNORM (ScreenSpace.hlsli EncodeScreenNormal)
	float2 Velocity : SV_Target1; // R16G16_FLOAT, UV 단위 현재 - 이전
};

FPrepassOutput MakePrepassOutput(FPixelInput Input, bool bFrontFace, FMaterialSurface Material)
{
	FPrepassOutput Output;
	const float  Roughness = Material.Roughness;
	const float3 Normal    = normalize(Input.WorldNormal);
	Output.Normal   = EncodeScreenNormal(bFrontFace ? Normal : -Normal, Roughness); // 양면 뒷면은 뒤집은 법선 (메인 패스와 같게)
	Output.Velocity = ComputeVelocity(Input.CurrentClip, Input.PreviousClip);
	return Output;
}

FPrepassOutput PSPrepass(FPixelInput Input, bool bFrontFace : SV_IsFrontFace)
{
	return MakePrepassOutput(Input, bFrontFace, EvaluateMeshMaterial(Input, bFrontFace));
}

// Masked: 메인 패스와 같은 알파 판정으로 잘라 깊이를 쓰지 않는다 (메인은 깊이 같음 테스트라 같은 픽셀만 남는다)
FPrepassOutput PSPrepassMasked(FPixelInput Input, bool bFrontFace : SV_IsFrontFace)
{
	const FMaterialSurface Material = EvaluateMeshMaterial(Input, bFrontFace);
	clip(Material.OpacityMask - E_MATERIAL_ALPHA_CUTOFF);
	return MakePrepassOutput(Input, bFrontFace, Material);
}

#ifndef E_LOCAL_LIGHTING_HLSLI
#define E_LOCAL_LIGHTING_HLSLI

// 클러스터 로컬 라이트(점광원/스포트/면광원) 평가 — 메시 패스(Mesh.hlsl, 지형·스프라이트 포함)와 물(Water.hlsl) 공용.
// 포함하기 전에: Lighting.hlsli(클러스터 상수), PBR.hlsli(FSurface, EvaluateDirectLight), 비교 샘플러 ShadowSampler,
//   E_LIGHT_SAMPLER_CLAMP(선형 클램프 — LTC·IES) / E_LIGHT_SAMPLER_WRAP(반복 — 쿠키).
// 레지스터 기본값 = 메시 루트 (t9~t12, 공간 3 표 — SceneRenderer RootParam_*). 다른 루트는 포함 전에 매크로로 바꾼다
#ifndef E_LOCAL_LIGHTS_REGISTER
#define E_LOCAL_LIGHTS_REGISTER t9
#endif
#ifndef E_CLUSTER_DATA_REGISTER
#define E_CLUSTER_DATA_REGISTER t10
#endif
#ifndef E_LOCAL_SHADOW_MATRICES_REGISTER
#define E_LOCAL_SHADOW_MATRICES_REGISTER t11
#endif
#ifndef E_LOCAL_SHADOW_MAP_REGISTER
#define E_LOCAL_SHADOW_MAP_REGISTER t12
#endif

// 점광원/스포트라이트 (LocalLightRenderer: 목록 + 클러스터별 인덱스)
StructuredBuffer<FLocalLight> LocalLights : register(E_LOCAL_LIGHTS_REGISTER);
StructuredBuffer<uint>        ClusterData : register(E_CLUSTER_DATA_REGISTER);
StructuredBuffer<float4x4>    LocalShadowMatrices : register(E_LOCAL_SHADOW_MATRICES_REGISTER); // 그림자 장별 뷰-투영
Texture2DArray<float>         LocalShadowMap      : register(E_LOCAL_SHADOW_MAP_REGISTER); // 그림자 타일 배열 (스포트·면광원 1장, 점광원 6장: +X,-X,+Y,-Y,+Z,-Z)

// 면광원 LTC 표·IES·쿠키 (Phase 52): 셰이더 가시 힙 전체 (루트 #26, 지형도 같은 공간 3) — 칸 번호는 라이트 목록/클러스터 상수
Texture2D LightTextures[] : register(t0, space3);
#define E_LIGHT_TEXTURE(Index) LightTextures[NonUniformResourceIndex(Index)]
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

#endif // E_LOCAL_LIGHTING_HLSLI

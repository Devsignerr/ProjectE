#include "Common.hlsli"
#define E_CLUSTER_CONSTANTS_REGISTER b2
#include "Lighting.hlsli"
#define E_SHADOW_CONSTANTS_REGISTER b1
#include "ShadowCommon.hlsli"

// 볼류메트릭 안개 (FFogRenderer, 식은 Renderer/FogMath.h): 프러스텀 정렬 3D 격자(화면 1/8 × 64조각, 깊이 = 거리 · w²)
//   CSInject   : 칸마다 밀도(높이 지수) + 들어오는 빛(방향광 × CSM 그림자 × HG 위상 + 안개 색 환경광 + 점광원/스포트) → (σs·L, σt).
//                조각 안 표본 위치를 프레임마다 흔들고 이전 프레임 결과를 재투영해 섞는다 (시간 누적)
//   CSIntegrate: 화면 칸마다 앞 → 뒤로 적분 → (누적 산란, 투과율). 적용은 Fog.hlsli SampleVolumetricFog

// ShaderTypes.h FVolumetricFogConstants와 1:1
cbuffer VolumetricConstants : register(b0)
{
	float4x4 InvViewProjection;  // 지터 없음
	float4x4 PrevViewProjection; // 지터 없음
	float3   CameraPosition;
	float    VolumetricDistance;
	float3   CameraForward;
	float    Density;           // 1/cm
	float3   LightDirection;    // 빛 진행 방향
	float    HeightFalloff;     // 1/cm
	float3   LightColor;        // 색 × 강도
	float    BaseHeight;
	float3   Albedo;
	float    ExtinctionScale;
	float3   AmbientColor;
	float    Anisotropy;
	uint     GridX;
	uint     GridY;
	uint     GridZ;
	float    SliceJitter;
	float    DirectionalScale;
	float    LocalLightScale;
	float    HistoryWeight;
	uint     bHistoryValid;
};

Texture2DArray<float>      ShadowMap     : register(t0);
StructuredBuffer<FLocalLight> LocalLights : register(t1);
Texture3D<float4>          HistoryVolume : register(t3); // 이전 프레임 주입 결과
Texture3D<float4>          InjectVolume  : register(t4); // 이번 프레임 주입 결과 (적분 입력)
RWTexture3D<float4>        OutputVolume  : register(u0);
SamplerState               LinearSampler : register(s0);
SamplerComparisonState     ShadowSampler : register(s1);

static const float FogPi = 3.14159265f;

float HenyeyGreenstein(float G, float CosTheta)
{
	const float G2    = G * G;
	const float Denom = max(1.0f + G2 - 2.0f * G * CosTheta, 1.0e-4f);
	return (1.0f - G2) / (4.0f * FogPi * Denom * sqrt(Denom));
}

float SliceToDepth(float W)
{
	return VolumetricDistance * W * W;
}

// 격자 칸 (x, y) 중심의 월드 광선 방향 (카메라 → 먼 평면)
float3 GetRayDirection(float2 UV)
{
	const float4 Far = mul(float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, 1.0f, 1.0f), InvViewProjection);
	return normalize(Far.xyz / Far.w - CameraPosition);
}

float SampleDirectionalShadow(float3 WorldPosition, float ViewDepth)
{
	if (ShadowEnabled < 0.5f)
	{
		return 1.0f;
	}
	const uint Cascade = SelectCascadeByDepth(ViewDepth);
	if (Cascade >= CascadeCount)
	{
		return 1.0f;
	}
	const float4 Clip = mul(float4(WorldPosition, 1.0f), CascadeViewProjection[Cascade]);
	const float2 UV   = Clip.xy * float2(0.5f, -0.5f) + 0.5f;
	if (any(UV < 0.0f) || any(UV > 1.0f) || Clip.z > 1.0f)
	{
		return 1.0f;
	}
	return ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV, Cascade), Clip.z);
}

[numthreads(4, 4, 4)]
void CSInject(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id >= uint3(GridX, GridY, GridZ)))
	{
		return;
	}
	const float2 UV        = (float2(Id.xy) + 0.5f) / float2(GridX, GridY);
	const float3 Dir       = GetRayDirection(UV);
	const float  W         = (Id.z + SliceJitter) / GridZ;
	const float  ViewDepth = SliceToDepth(W);
	const float3 P         = CameraPosition + Dir * (ViewDepth / max(dot(Dir, CameraForward), 1.0e-3f));

	const float Extinction = max(Density * exp(-HeightFalloff * (P.z - BaseHeight)), 0.0f) * ExtinctionScale;

	// 들어오는 빛 (카메라로 나가는 방향 = -Dir)
	float3 Light = AmbientColor;
	{
		const float Phase  = HenyeyGreenstein(Anisotropy, dot(LightDirection, -Dir));
		const float Shadow = SampleDirectionalShadow(P, ViewDepth);
		Light += LightColor * (Phase * 4.0f * FogPi * Shadow * DirectionalScale); // 위상 평균 1 기준 (고른 산란이면 1배)
	}
	const uint LightCount = min(LocalLightCount, 128u);
	for (uint Index = 0; Index < LightCount; ++Index)
	{
		const FLocalLight Local    = LocalLights[Index];
		const float3      ToLight  = Local.Position - P;
		const float       Distance = length(ToLight);
		if (Distance >= Local.Radius)
		{
			continue;
		}
		const float3 L     = ToLight / max(Distance, 1.0e-3f);
		const float  Atten = LightDistanceAttenuation(Distance, Local.Radius) * LightConeAttenuation(dot(Local.Direction, -L), Local.ConeScale, Local.ConeOffset);
		Light += Local.Color * (Atten * HenyeyGreenstein(Anisotropy, dot(-L, -Dir)) * 4.0f * FogPi * LocalLightScale);
	}
	float4 Value = float4(Light * Albedo * Extinction, Extinction);

	// 시간 누적: 이 점의 이전 프레임 격자 좌표 (조각 깊이 역함수 = sqrt)
	if (bHistoryValid != 0)
	{
		const float4 PrevClip = mul(float4(P, 1.0f), PrevViewProjection);
		if (PrevClip.w > 1.0e-3f)
		{
			const float2 PrevUV = float2(PrevClip.x / PrevClip.w * 0.5f + 0.5f, 0.5f - PrevClip.y / PrevClip.w * 0.5f);
			const float  PrevW  = sqrt(saturate(PrevClip.w / max(VolumetricDistance, 1.0f))); // 원근: w = 뷰 깊이
			if (all(PrevUV >= 0.0f) && all(PrevUV <= 1.0f) && PrevW < 1.0f)
			{
				const float4 History = HistoryVolume.SampleLevel(LinearSampler, float3(PrevUV, PrevW), 0.0f);
				Value                = lerp(Value, History, HistoryWeight);
			}
		}
	}
	OutputVolume[Id] = Value;
}

[numthreads(8, 8, 1)]
void CSIntegrate(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= uint2(GridX, GridY)))
	{
		return;
	}
	const float2 UV       = (float2(Id.xy) + 0.5f) / float2(GridX, GridY);
	const float3 Dir      = GetRayDirection(UV);
	const float  RayScale = 1.0f / max(dot(Dir, CameraForward), 1.0e-3f); // 뷰 깊이 → 광선 길이

	float3 Scattering    = 0.0f;
	float  Transmittance = 1.0f;
	for (uint Z = 0; Z < GridZ; ++Z)
	{
		const float4 Value   = InjectVolume.Load(int4(Id.xy, Z, 0));
		const float  Length  = (SliceToDepth((Z + 1.0f) / GridZ) - SliceToDepth((float)Z / GridZ)) * RayScale;
		const float  SliceT  = exp(-max(Value.a, 0.0f) * Length);
		// 에너지 보존 적분 (Hillaire 2015): S · (1 - T) / σ
		const float3 Integrated = Value.a > 1.0e-7f ? (Value.rgb - Value.rgb * SliceT) / Value.a : Value.rgb * Length;
		Scattering += Integrated * Transmittance;
		Transmittance *= SliceT;
		OutputVolume[uint3(Id.xy, Z)] = float4(Scattering, Transmittance);
	}
}

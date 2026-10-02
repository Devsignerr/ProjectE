#ifndef E_FOG_HLSLI
#define E_FOG_HLSLI

// 안개 적용 공용 (FogApply.hlsl 전체 화면 / Particle.hlsl 정점). 식은 Renderer/FogMath.h와 같다 (테스트 FogTests)
//   해석식 높이 안개는 [max(시작 거리, 볼류메트릭 거리), 표면] 구간, 볼류메트릭(3D 격자 적분 결과)은 [0, 볼류메트릭 거리] 구간
//   합성: 색 = (원래 × T해석 + I해석) × T볼륨 + I볼륨
//   레지스터는 포함하는 쪽이 E_FOG_CONSTANTS_REGISTER / E_FOG_VOLUME_REGISTER로 바꾼다 (적용 패스 b0/t1, 파티클 b2/t2)

#ifndef E_FOG_CONSTANTS_REGISTER
#define E_FOG_CONSTANTS_REGISTER b0
#endif
#ifndef E_FOG_VOLUME_REGISTER
#define E_FOG_VOLUME_REGISTER t1
#endif
#ifndef E_FOG_SAMPLER_REGISTER
#define E_FOG_SAMPLER_REGISTER s0
#endif

// ShaderTypes.h FFogConstants와 1:1
cbuffer FogConstants : register(E_FOG_CONSTANTS_REGISTER)
{
	float3   FogColor;
	float    FogDensity;       // 1/cm
	float3   FogDirectionalColor;
	float    FogHeightFalloff; // 1/cm
	float3   FogLightDirection;
	float    FogBaseHeight;
	float3   FogCameraPosition;
	float    FogStartDistance;
	float3   FogCameraForward;
	float    FogMaxOpacity;
	float    FogDirectionalExponent;
	float    FogDirectionalStartDistance;
	uint     FogEnabled;
	uint     FogVolumetric;
	float    FogVolumetricDistance;
	float    FogSkyDistance;
	float2   FogScreenSize;
	float4x4 FogViewProjection;    // 지터 없음
	float4x4 FogInvViewProjection; // 지터 포함 투영의 역
	// 공중 원근 (Phase 49, AerialPerspective.hlsli — 대기 컴포넌트가 있을 때만 FogAerialEnabled)
	float3   FogAerialRayleighScattering;
	float    FogAerialRayleighScaleHeight;
	float3   FogAerialMieScattering;
	float    FogAerialMieScaleHeight;
	float3   FogAerialMieExtinction;
	float    FogAerialMieAnisotropy;
	float3   FogAerialSunIlluminance;
	uint     FogAerialEnabled;
	float3   FogAerialSunDirection;
	float    FogAerialGroundHeight;
	float3   FogAerialMultiScattering;
	float    FogAerialDistanceScale;
};

#include "AerialPerspective.hlsli"

Texture3D<float4> FogVolume        : register(E_FOG_VOLUME_REGISTER); // RGB = 누적 산란, A = 투과율 (조각 끝까지)
SamplerState      FogLinearSampler : register(E_FOG_SAMPLER_REGISTER);

static const uint E_FOG_VOLUME_SLICES = 64; // FFogMath::VolumeSliceCount

float FogHeightOpticalDepth(float OriginZ, float DirZ, float Start, float End)
{
	if (End <= Start || FogDensity <= 0.0f)
	{
		return 0.0f;
	}
	const float Length   = End - Start;
	const float StartZ   = OriginZ + DirZ * Start;
	const float AtStart  = FogDensity * exp(-FogHeightFalloff * (StartZ - FogBaseHeight));
	const float Exponent = FogHeightFalloff * DirZ * Length;
	if (abs(Exponent) < 1.0e-4f)
	{
		return AtStart * Length * (1.0f - 0.5f * Exponent);
	}
	return AtStart * (1.0f - exp(-Exponent)) / (FogHeightFalloff * DirZ);
}

// 해석식 높이 안개: (산란 색 × 안개 양, 투과율)
float4 EvaluateHeightFog(float3 WorldPosition)
{
	const float3 ToPoint  = WorldPosition - FogCameraPosition;
	const float  Distance = length(ToPoint);
	const float3 Dir      = ToPoint / max(Distance, 1.0e-3f);
	const float  Start    = max(FogStartDistance, FogVolumetric != 0 ? FogVolumetricDistance : 0.0f);
	const float  Depth    = FogHeightOpticalDepth(FogCameraPosition.z, Dir.z, Start, Distance);
	const float  Amount   = min(1.0f - exp(-max(Depth, 0.0f)), saturate(FogMaxOpacity));
	float3       Color    = FogColor;
	if (Distance > FogDirectionalStartDistance)
	{
		Color += FogDirectionalColor * pow(saturate(-dot(Dir, FogLightDirection)), max(FogDirectionalExponent, 1.0f));
	}
	return float4(Color * Amount, 1.0f - Amount);
}

// 볼류메트릭 결과 (누적 산란, 투과율). 볼륨 거리보다 먼 점은 마지막 조각
float4 SampleVolumetricFog(float3 WorldPosition)
{
	const float4 Clip = mul(float4(WorldPosition, 1.0f), FogViewProjection);
	if (Clip.w <= 1.0e-4f)
	{
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}
	const float2 UV        = float2(Clip.x / Clip.w * 0.5f + 0.5f, 0.5f - Clip.y / Clip.w * 0.5f);
	const float  ViewDepth = dot(WorldPosition - FogCameraPosition, FogCameraForward);
	const float  W         = sqrt(saturate(ViewDepth / max(FogVolumetricDistance, 1.0f))); // FFogMath::DepthToSlice
	// 칸 z 값 = 조각 z 끝까지의 적분 → 깊이 w의 값은 텍셀 좌표 w·N - 0.5 (가까운 첫 조각 안은 0으로 보간)
	const float  Z         = (W * E_FOG_VOLUME_SLICES - 0.5f) / E_FOG_VOLUME_SLICES;
	const float4 Value     = FogVolume.SampleLevel(FogLinearSampler, float3(saturate(UV), max(Z, 0.5f / E_FOG_VOLUME_SLICES)), 0.0f);
	const float  NearBlend = saturate(W * E_FOG_VOLUME_SLICES); // 첫 조각 안쪽은 카메라 쪽(산란 0, 투과 1)과 섞는다
	return lerp(float4(0.0f, 0.0f, 0.0f, 1.0f), Value, NearBlend);
}

// 높이 + 볼류메트릭 안개만 (더할 산란, 곱할 투과율) — 하늘 픽셀(FogApply)은 공중 원근 없이 이것만
float4 EvaluateHeightAndVolumetricFog(float3 WorldPosition)
{
	if (FogEnabled == 0)
	{
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}
	const float4 Height = EvaluateHeightFog(WorldPosition);
	if (FogVolumetric == 0)
	{
		return Height;
	}
	const float4 Volume = SampleVolumetricFog(WorldPosition);
	return float4(Height.rgb * Volume.a + Volume.rgb, Height.a * Volume.a);
}

// 공중 원근 (Phase 49): 투과율은 합성 경로가 스칼라 알파라 휘도 가중 평균 하나 (FAtmosphereMath::ComputeAerialTransmittanceScalar)
float4 EvaluateAerialFog(float3 WorldPosition)
{
	FAerialPerspectiveInputs Inputs;
	Inputs.RayleighScattering  = FogAerialRayleighScattering;
	Inputs.RayleighScaleHeight = FogAerialRayleighScaleHeight;
	Inputs.MieScattering       = FogAerialMieScattering;
	Inputs.MieScaleHeight      = FogAerialMieScaleHeight;
	Inputs.MieExtinction       = FogAerialMieExtinction;
	Inputs.MieAnisotropy       = FogAerialMieAnisotropy;
	Inputs.SunIlluminance      = FogAerialSunIlluminance;
	Inputs.SunDirection        = FogAerialSunDirection;
	Inputs.MultiScattering     = FogAerialMultiScattering;
	Inputs.GroundHeight        = FogAerialGroundHeight;
	Inputs.DistanceScale       = FogAerialDistanceScale;
	float3       Transmittance;
	const float3 Inscatter = EvaluateAerialPerspective(Inputs, FogCameraPosition, WorldPosition, Transmittance);
	return float4(Inscatter, dot(Transmittance, float3(0.2126f, 0.7152f, 0.0722f)));
}

// 합성 결과 (더할 산란, 곱할 투과율). 시선 순서: 카메라 ← 볼류메트릭(가까움) ← 높이 안개 ← 공중 원근(멀리) ← 표면
//   색 = ((표면 × T공중 + S공중) × T높이 + S높이) × T볼륨 + S볼륨
float4 EvaluateFog(float3 WorldPosition)
{
	float4 Fog = EvaluateHeightAndVolumetricFog(WorldPosition);
	if (FogAerialEnabled != 0)
	{
		const float4 Aerial = EvaluateAerialFog(WorldPosition);
		Fog                 = float4(Aerial.rgb * Fog.a + Fog.rgb, Aerial.a * Fog.a);
	}
	return Fog;
}

#endif // E_FOG_HLSLI

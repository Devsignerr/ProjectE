#ifndef E_PBR_HLSLI
#define E_PBR_HLSLI

#include "Common.hlsli"

// 금속/거칠기 PBR (Cook-Torrance). 모든 값은 선형 HDR.
//   D: GGX(Trowbridge-Reitz), V: Smith-GGX height-correlated, F: Schlick
//   확산: Lambert * (1 - F) * (1 - Metallic)

struct FSurface
{
	float3 Albedo;    // 선형 베이스 컬러
	float  Metallic;
	float  Roughness; // 지각적 거칠기 (알파 = Roughness^2)
	float3 N;         // 월드 법선 (정규화)
	float3 V;         // 표면 → 카메라 (정규화)
	float  Occlusion; // AO (1 = 가림 없음)
};

float3 GetF0(FSurface Surface)
{
	return lerp(float3(0.04f, 0.04f, 0.04f), Surface.Albedo, Surface.Metallic);
}

float D_GGX(float NdotH, float Alpha)
{
	const float A2    = Alpha * Alpha;
	const float Denom = NdotH * NdotH * (A2 - 1.0f) + 1.0f;
	return A2 / max(E_PI * Denom * Denom, 1.0e-7f);
}

// Heitz 2014 height-correlated Smith. G / (4 NdotL NdotV)까지 포함한 가시성 항
float V_SmithGGXCorrelated(float NdotV, float NdotL, float Alpha)
{
	const float A2   = Alpha * Alpha;
	const float GgxV = NdotL * sqrt(NdotV * NdotV * (1.0f - A2) + A2);
	const float GgxL = NdotV * sqrt(NdotL * NdotL * (1.0f - A2) + A2);
	return 0.5f / max(GgxV + GgxL, 1.0e-7f);
}

float3 F_Schlick(float3 F0, float VdotH)
{
	const float Fc = pow(1.0f - VdotH, 5.0f);
	return F0 + (1.0f - F0) * Fc;
}

// 한 광원의 기여 (Radiance = 색 * 강도, L = 표면 → 광원)
float3 EvaluateDirectLight(FSurface Surface, float3 L, float3 Radiance)
{
	const float NdotL = saturate(dot(Surface.N, L));
	if (NdotL <= 0.0f)
	{
		return 0.0f;
	}

	const float3 H     = normalize(L + Surface.V);
	const float  NdotV = max(dot(Surface.N, Surface.V), 1.0e-4f);
	const float  NdotH = saturate(dot(Surface.N, H));
	const float  VdotH = saturate(dot(Surface.V, H));
	const float  Alpha = max(Surface.Roughness * Surface.Roughness, 2.0e-3f);

	const float3 F        = F_Schlick(GetF0(Surface), VdotH);
	const float3 Specular = D_GGX(NdotH, Alpha) * V_SmithGGXCorrelated(NdotV, NdotL, Alpha) * F;
	const float3 Diffuse  = (1.0f - F) * (1.0f - Surface.Metallic) * Surface.Albedo / E_PI;

	return (Diffuse + Specular) * Radiance * NdotL;
}

// Karis, "Physically Based Shading on Mobile" — 분석적 환경 BRDF 근사 (분할 합의 스케일/바이어스)
float3 EnvBRDFApprox(float3 SpecularColor, float Roughness, float NdotV)
{
	const float4 C0   = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
	const float4 C1   = float4(1.0f, 0.0425f, 1.04f, -0.04f);
	const float4 R    = Roughness * C0 + C1;
	const float  A004 = min(R.x * R.x, exp2(-9.28f * NdotV)) * R.x + R.y;
	const float2 AB   = float2(-1.04f, 1.04f) * A004 + R.zw;
	return SpecularColor * AB.x + AB.y;
}

// 하늘/지면 그라디언트 (월드 +Z 위). 방향 D에서 들어오는 복사휘도 근사
float3 SampleSkyGradient(float3 D, float3 SkyColor, float3 GroundColor)
{
	return lerp(GroundColor, SkyColor, saturate(D.z * 0.5f + 0.5f));
}

// 간이 환경광: 반구 확산 + 반사 방향 하늘 그라디언트(거칠기로 평균 쪽으로 흐림) * 환경 BRDF.
// 이후 IBL(조도 맵 + 사전 필터 환경 맵 + BRDF LUT)이 이 함수를 대체한다.
float3 EvaluateAmbient(FSurface Surface, float3 SkyColor, float3 GroundColor, float Intensity)
{
	const float  NdotV = max(dot(Surface.N, Surface.V), 1.0e-4f);
	const float3 F0    = GetF0(Surface);

	const float3 Irradiance = SampleSkyGradient(Surface.N, SkyColor, GroundColor);
	const float3 Diffuse    = Irradiance * Surface.Albedo * (1.0f - Surface.Metallic);

	const float3 R           = reflect(-Surface.V, Surface.N);
	const float3 Prefiltered = lerp(SampleSkyGradient(R, SkyColor, GroundColor), 0.5f * (SkyColor + GroundColor), Surface.Roughness);
	const float3 Specular    = Prefiltered * EnvBRDFApprox(F0, Surface.Roughness, NdotV);

	return (Diffuse + Specular) * Surface.Occlusion * Intensity;
}

#endif // E_PBR_HLSLI

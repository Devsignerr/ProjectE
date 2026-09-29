#include "Common.hlsli"
#include "Fullscreen.hlsli"

// HDR 씬 (+ 블룸) → 노출 → 톤매핑 → 표시용. 출력 RTV가 sRGB이므로 선형 값을 쓴다 (감마 인코딩은 하드웨어가 처리)

cbuffer TonemapConstants : register(b0)
{
	float  ExposureEV;        // 수동 노출 / 자동 노출 보정 (스톱)
	uint   TonemapOperator;   // 0 = 없음(클램프), 1 = ACES 근사(Narkowicz), 2 = Reinhard
	float  BloomIntensity;    // 0이면 블룸 없음
	uint   bAutoExposure;
	float  AutoExposureMinEV;
	float  AutoExposureMaxEV;
	float2 Padding0;
};

Texture2D<float4>         SceneColor       : register(t0);
Texture2D<float4>         BloomColor       : register(t1);
RWStructuredBuffer<float> AdaptedLuminance : register(u1);
SamplerState              LinearSampler    : register(s0);
SamplerState              PointSampler     : register(s1);

float3 TonemapAcesApprox(float3 X)
{
	// Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve"
	const float A = 2.51f;
	const float B = 0.03f;
	const float C = 2.43f;
	const float D = 0.59f;
	const float E = 0.14f;
	return saturate((X * (A * X + B)) / (X * (C * X + D) + E));
}

float3 TonemapReinhard(float3 X)
{
	return X / (1.0f + X);
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSMain(FFullscreenVSOutput Input) : SV_Target
{
	float3 Hdr = SceneColor.SampleLevel(PointSampler, Input.UV, 0.0f).rgb;
	if (BloomIntensity > 0.0f)
	{
		Hdr += BloomColor.SampleLevel(LinearSampler, Input.UV, 0.0f).rgb * BloomIntensity;
	}

	float EV = ExposureEV;
	if (bAutoExposure != 0)
	{
		// FPostProcessMath::ComputeAutoExposureEV와 같은 식 (18% 회색 기준)
		const float AverageLuminance = max(AdaptedLuminance[0], 1.0e-4f);
		EV += clamp(log2(0.18f / AverageLuminance), AutoExposureMinEV, AutoExposureMaxEV);
	}
	Hdr *= exp2(EV);

	float3 Ldr;
	if (TonemapOperator == 1)
	{
		Ldr = TonemapAcesApprox(Hdr);
	}
	else if (TonemapOperator == 2)
	{
		Ldr = TonemapReinhard(Hdr);
	}
	else
	{
		Ldr = saturate(Hdr);
	}
	return float4(Ldr, 1.0f);
}

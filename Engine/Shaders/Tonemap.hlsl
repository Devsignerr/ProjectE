#include "Common.hlsli"
#include "Fullscreen.hlsli"

// HDR 씬 → 표시용 LDR. 출력 RTV가 sRGB이므로 선형 값을 쓴다 (감마 인코딩은 하드웨어가 처리)

cbuffer TonemapConstants : register(b0)
{
	float Exposure;       // 노출 배율 (2^EV)
	uint  TonemapOperator; // 0 = 없음(클램프), 1 = ACES 근사(Narkowicz), 2 = Reinhard
	float2 Padding0;
};

Texture2D<float4> SceneColor    : register(t0);
SamplerState      PointSampler  : register(s0);

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
	const float3 Hdr = SceneColor.SampleLevel(PointSampler, Input.UV, 0.0f).rgb * Exposure;

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

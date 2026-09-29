#include "Common.hlsli"
#include "Fullscreen.hlsli"

// 블룸 (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare")
//   PSDownsample: 13탭 다운샘플. 첫 단계(bFirstPass)는 Karis 평균으로 반딧불 억제 + 소프트 니 임계값 추출
//   PSUpsample  : 3x3 텐트 필터 업샘플, PSO 가산 블렌드로 한 단계 큰 레벨에 누적

cbuffer BloomConstants : register(b0)
{
	float2 SourceTexelSize; // 1 / 원본 크기
	uint   bFirstPass;
	float  Threshold;
	float  Knee;
	float  FilterRadius;    // 업샘플 텐트 반경 (원본 텍셀 단위)
	float2 Padding0;
};

Texture2D<float4> Source        : register(t0);
SamplerState      LinearSampler : register(s0);

float Luminance(float3 Color)
{
	return dot(Color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Karis 평균 가중치: 밝은 표본의 영향을 줄여 깜빡이는 점광(반딧불)을 억제
float KarisWeight(float3 Color)
{
	return 1.0f / (1.0f + Luminance(Color));
}

// 소프트 니 임계값 (CPU FPostProcessMath::ComputeBloomContribution과 같은 식)
float3 ApplyThreshold(float3 Color)
{
	const float Brightness = max(Color.r, max(Color.g, Color.b));
	const float SoftKnee   = Threshold * Knee + 1.0e-5f;
	float       Soft       = clamp(Brightness - Threshold + SoftKnee, 0.0f, 2.0f * SoftKnee);
	Soft                   = Soft * Soft / (4.0f * SoftKnee);
	const float Contrib    = max(Soft, Brightness - Threshold);
	return Color * (Contrib / max(Brightness, 1.0e-5f));
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float3 SampleSource(float2 UV, float2 OffsetTexels)
{
	return Source.SampleLevel(LinearSampler, UV + OffsetTexels * SourceTexelSize, 0.0f).rgb;
}

float4 PSDownsample(FFullscreenVSOutput Input) : SV_Target
{
	const float2 UV = Input.UV;

	// 13탭: 중앙 4개(내부 박스) + 바깥 격자 9개
	const float3 A = SampleSource(UV, float2(-2.0f, -2.0f));
	const float3 B = SampleSource(UV, float2( 0.0f, -2.0f));
	const float3 C = SampleSource(UV, float2( 2.0f, -2.0f));
	const float3 D = SampleSource(UV, float2(-1.0f, -1.0f));
	const float3 E = SampleSource(UV, float2( 1.0f, -1.0f));
	const float3 F = SampleSource(UV, float2(-2.0f,  0.0f));
	const float3 G = SampleSource(UV, float2( 0.0f,  0.0f));
	const float3 H = SampleSource(UV, float2( 2.0f,  0.0f));
	const float3 I = SampleSource(UV, float2(-1.0f,  1.0f));
	const float3 J = SampleSource(UV, float2( 1.0f,  1.0f));
	const float3 K = SampleSource(UV, float2(-2.0f,  2.0f));
	const float3 L = SampleSource(UV, float2( 0.0f,  2.0f));
	const float3 M = SampleSource(UV, float2( 2.0f,  2.0f));

	// 5개의 2x2 박스 그룹: 중앙(가중 0.5) + 네 모서리(각 0.125)
	const float3 Group0 = (D + E + I + J) * 0.25f;
	const float3 Group1 = (A + B + F + G) * 0.25f;
	const float3 Group2 = (B + C + G + H) * 0.25f;
	const float3 Group3 = (F + G + K + L) * 0.25f;
	const float3 Group4 = (G + H + L + M) * 0.25f;

	float3 Result;
	if (bFirstPass != 0)
	{
		const float W0 = KarisWeight(Group0) * 0.5f;
		const float W1 = KarisWeight(Group1) * 0.125f;
		const float W2 = KarisWeight(Group2) * 0.125f;
		const float W3 = KarisWeight(Group3) * 0.125f;
		const float W4 = KarisWeight(Group4) * 0.125f;
		Result = (Group0 * W0 + Group1 * W1 + Group2 * W2 + Group3 * W3 + Group4 * W4) / max(W0 + W1 + W2 + W3 + W4, 1.0e-5f);
		Result = ApplyThreshold(Result);
	}
	else
	{
		Result = Group0 * 0.5f + (Group1 + Group2 + Group3 + Group4) * 0.125f;
	}
	return float4(max(Result, 0.0f), 1.0f);
}

float4 PSUpsample(FFullscreenVSOutput Input) : SV_Target
{
	const float2 UV = Input.UV;
	const float  R  = FilterRadius;

	// 3x3 텐트: 1 2 1 / 2 4 2 / 1 2 1  (합 16)
	float3 Sum = SampleSource(UV, float2(0.0f, 0.0f)) * 4.0f;
	Sum += (SampleSource(UV, float2(-R, 0.0f)) + SampleSource(UV, float2(R, 0.0f)) +
	        SampleSource(UV, float2(0.0f, -R)) + SampleSource(UV, float2(0.0f, R))) * 2.0f;
	Sum += SampleSource(UV, float2(-R, -R)) + SampleSource(UV, float2(R, -R)) +
	       SampleSource(UV, float2(-R, R)) + SampleSource(UV, float2(R, R));
	return float4(Sum * (1.0f / 16.0f), 1.0f);
}

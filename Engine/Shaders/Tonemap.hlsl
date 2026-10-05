#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "HdrDisplay.hlsli"

// HDR 씬 (+ 블룸) → 노출 → 톤매핑 → 표시용. 출력 RTV가 sRGB이므로 선형 값을 쓴다 (감마 인코딩은 하드웨어가 처리)

cbuffer TonemapConstants : register(b0)
{
	float  ExposureEV;        // 수동 노출 / 자동 노출 보정 (스톱)
	uint   TonemapOperator;   // 0 = 없음(클램프), 1 = ACES 근사(Narkowicz), 2 = Reinhard
	float  BloomIntensity;    // 0이면 블룸 없음
	uint   bAutoExposure;
	float  AutoExposureMinEV;
	float  AutoExposureMaxEV;
	float  Sharpness;         // TAA 흐림 보정 (0 = 끔): 4이웃 언샤프 마스크, 톤매핑 공간 대비로 제한
	float  HdrPeakRatio;      // HDR 출력 (Phase 49): 최대 밝기 / 종이 흰색 (0 = SDR — 기존 출력 그대로)
	// 색 보정·비네트 (Renderer/ColorGradingMath.h — 톤매핑 직후 SDR [0, 1], HDR 하이라이트 펼치기 전)
	uint   bColorGrading;      // t2 = 1024x32 LUT 띠 (sRGB 인코딩 인덱스·값)
	float  VignetteIntensity;  // 0 = 없음
	float  VignetteSize;
	float  VignetteSmoothness;
	float3 VignetteColor;      // 선형
	float  VignetteRoundness;
	float  VignetteAspect;     // 너비 / 높이
};

Texture2D<float4>         SceneColor       : register(t0);
Texture2D<float4>         BloomColor       : register(t1);
Texture2D<float4>         GradingLut       : register(t2);
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

float SrgbEncodeChannel(float C)
{
	return C <= 0.0031308f ? C * 12.92f : 1.055f * pow(C, 1.0f / 2.4f) - 0.055f;
}

float SrgbDecodeChannel(float C)
{
	return C <= 0.04045f ? C / 12.92f : pow((C + 0.055f) / 1.055f, 2.4f);
}

// 색 보정 LUT (ColorGradingMath::BakeLut 띠): 인코딩 색 → 칸 좌표, 파랑 두 칸을 쌍선형으로 읽어 보간
float3 ApplyColorGrading(float3 Linear)
{
	const float  Size    = 32.0f;
	const float3 Encoded = float3(SrgbEncodeChannel(saturate(Linear.r)), SrgbEncodeChannel(saturate(Linear.g)), SrgbEncodeChannel(saturate(Linear.b)));
	const float3 Cell    = Encoded * (Size - 1.0f);
	const float  Slice   = min(floor(Cell.b), Size - 2.0f);
	const float  Blend   = Cell.b - Slice;
	const float2 UV0     = float2((Cell.r + 0.5f + Slice * Size) / (Size * Size), (Cell.g + 0.5f) / Size);
	const float2 UV1     = UV0 + float2(1.0f / Size, 0.0f);
	const float3 Graded  = lerp(GradingLut.SampleLevel(LinearSampler, UV0, 0.0f).rgb, GradingLut.SampleLevel(LinearSampler, UV1, 0.0f).rgb, Blend);
	return float3(SrgbDecodeChannel(Graded.r), SrgbDecodeChannel(Graded.g), SrgbDecodeChannel(Graded.b));
}

// ColorGradingMath::ComputeVignetteMask와 같은 식
float VignetteMask(float2 UV)
{
	const float  ScaleX = 1.0f + (VignetteAspect - 1.0f) * VignetteRoundness;
	const float2 P      = (UV - 0.5f) * 2.0f * float2(ScaleX, 1.0f);
	const float  R      = length(P) / sqrt(ScaleX * ScaleX + 1.0f);
	return smoothstep(VignetteSize, VignetteSize + VignetteSmoothness, R);
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSMain(FFullscreenVSOutput Input) : SV_Target
{
	float3 Hdr = SceneColor.SampleLevel(PointSampler, Input.UV, 0.0f).rgb;
	if (Sharpness > 0.0f)
	{
		// 밝기 비례 가중(1 / (1 + 최대 성분))으로 이웃 평균을 구해 아주 밝은 픽셀 주변에 링잉이 생기지 않게 한다
		const int2   Pixel = int2(Input.Position.xy);
		const float3 N     = SceneColor.Load(int3(Pixel + int2(0, -1), 0)).rgb;
		const float3 S     = SceneColor.Load(int3(Pixel + int2(0, 1), 0)).rgb;
		const float3 E     = SceneColor.Load(int3(Pixel + int2(1, 0), 0)).rgb;
		const float3 W     = SceneColor.Load(int3(Pixel + int2(-1, 0), 0)).rgb;
		const float3 Blur  = (N + S + E + W) * 0.25f;
		const float  Limit = 1.0f / (1.0f + max(Hdr.r, max(Hdr.g, Hdr.b)));
		Hdr                = max(Hdr + (Hdr - Blur) * Sharpness * Limit, 0.0f);
	}
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
	if (bColorGrading != 0)
	{
		Ldr = ApplyColorGrading(Ldr);
	}
	if (VignetteIntensity > 0.0f)
	{
		Ldr = lerp(Ldr, VignetteColor, VignetteMask(Input.UV) * VignetteIntensity);
	}
	if (HdrPeakRatio > 1.0f && TonemapOperator != 0)
	{
		// 톤매핑 없음(0, 2D·원본 색)은 펼치지 않는다 — HDR 출력에서도 1 = 종이 흰색 그대로 (텍스처 색 = 화면 색)
		// HDR 출력: SDR 곡선 값의 하이라이트만 펼친다 (무릎 아래 = SDR과 같은 값, 최대 채널 기준 비율 — 색상 유지). 출력 = 선형, 1 = 종이 흰색
		const float Peak = max(Ldr.r, max(Ldr.g, Ldr.b));
		if (Peak > 1.0e-5f)
		{
			Ldr *= HdrExpandHighlights(Peak, HdrPeakRatio, E_HDR_DEFAULT_KNEE) / Peak;
		}
	}
	return float4(Ldr, 1.0f);
}

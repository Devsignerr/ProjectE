#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// TAA 해상 (FTemporalAA, 톤매핑 전 HDR). 식은 Renderer/TemporalMath.h와 같다
//   1) 3x3 이웃에서 가장 가까운 깊이의 움직임 벡터 (윤곽선 고스팅 감소). 기하가 없으면(깊이 1) 카메라 재투영
//   2) 이전 UV = UV - 움직임. 이력을 Catmull-Rom(5탭)으로 읽는다
//   3) 이웃 색 분산(YCoCg, 톤매핑 공간) 상자로 이력을 클립 → 고스팅/가려짐 해제 대응
//   4) 현재 비중 = 기본값, 반응형 마스크(씬 컬러 알파 = 파티클 덮임)와 큰 움직임에서 키운다. 이력 없음이면 현재 그대로

cbuffer TaaConstants : register(b0)
{
	float4x4 Reprojection;    // 현재 클립(지터 없음) → 이전 클립, 카메라만 (기하 없는 픽셀)
	float2   TexelSize;       // 1 / 화면 크기
	float    CurrentWeight;   // 기본 현재 프레임 비중
	uint     bHistoryValid;
	float    ReactiveWeight;  // 반응형 마스크가 1일 때 현재 비중
	float    VarianceGamma;   // 이웃 상자 폭 (표준편차 배수)
	float2   Padding;
};

Texture2D<float4> SceneColor    : register(t0); // 알파 = 반응형 마스크 (불투명 0, 파티클 덮임)
Texture2D<float4> History       : register(t1);
Texture2D<float2> Velocity      : register(t2);
Texture2D<float>  Depth         : register(t3);
SamplerState      LinearSampler : register(s0);
SamplerState      PointSampler  : register(s1);

float MaxComponent(float3 C)
{
	return max(C.r, max(C.g, C.b));
}

// 밝은 픽셀 하나가 이력을 지배하지 않게 톤매핑 공간에서 섞는다 (가역)
float3 TonemapForTaa(float3 C)
{
	return C / (1.0f + MaxComponent(C));
}

float3 InverseTonemapForTaa(float3 C)
{
	return C / max(1.0f - MaxComponent(C), 1.0e-4f);
}

float3 RgbToYCoCg(float3 C)
{
	return float3(0.25f * C.r + 0.5f * C.g + 0.25f * C.b, 0.5f * C.r - 0.5f * C.b, -0.25f * C.r + 0.5f * C.g - 0.25f * C.b);
}

float3 YCoCgToRgb(float3 C)
{
	return float3(C.x + C.y - C.z, C.x + C.z, C.x - C.y - C.z);
}

// Catmull-Rom 9탭을 쌍선형 5탭으로 (모서리 4개 생략)
float3 SampleHistoryCatmullRom(float2 UV)
{
	const float2 SamplePos = UV / TexelSize;
	const float2 TexPos1   = floor(SamplePos - 0.5f) + 0.5f;
	const float2 F         = SamplePos - TexPos1;
	const float2 W0        = F * (-0.5f + F * (1.0f - 0.5f * F));
	const float2 W1        = 1.0f + F * F * (-2.5f + 1.5f * F);
	const float2 W2        = F * (0.5f + F * (2.0f - 1.5f * F));
	const float2 W3        = F * F * (-0.5f + 0.5f * F);
	const float2 W12       = W1 + W2;
	const float2 Offset12  = W2 / W12;
	const float2 P0        = (TexPos1 - 1.0f) * TexelSize;
	const float2 P3        = (TexPos1 + 2.0f) * TexelSize;
	const float2 P12       = (TexPos1 + Offset12) * TexelSize;

	float3 Result = 0.0f;
	float  Weight = 0.0f;
	const float W[5]  = { W12.x * W0.y, W0.x * W12.y, W12.x * W12.y, W3.x * W12.y, W12.x * W3.y };
	const float2 P[5] = { float2(P12.x, P0.y), float2(P0.x, P12.y), P12, float2(P3.x, P12.y), float2(P12.x, P3.y) };
	[unroll]
	for (int Index = 0; Index < 5; ++Index)
	{
		Result += History.SampleLevel(LinearSampler, P[Index], 0.0f).rgb * W[Index];
		Weight += W[Index];
	}
	return max(Result / max(Weight, 1.0e-4f), 0.0f);
}

// 이력을 상자 중심 방향으로 상자 안까지 당긴다 (AABB 클립)
float3 ClipToBox(float3 HistoryValue, float3 BoxMin, float3 BoxMax)
{
	const float3 Center = 0.5f * (BoxMax + BoxMin);
	const float3 Extent = 0.5f * (BoxMax - BoxMin) + 1.0e-5f;
	const float3 Offset = HistoryValue - Center;
	const float3 Units  = abs(Offset / Extent);
	const float  Scale  = max(Units.x, max(Units.y, Units.z));
	return Scale > 1.0f ? Center + Offset / Scale : HistoryValue;
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSResolve(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel   = int2(Input.Position.xy);
	const float4 Center  = SceneColor.Load(int3(Pixel, 0));
	const float3 Current = TonemapForTaa(max(Center.rgb, 0.0f));

	// 이웃 통계 + 가장 가까운 깊이
	float3 Mean        = 0.0f;
	float3 MeanSquared = 0.0f;
	float  Closest     = 1.0f;
	int2   ClosestPixel = Pixel;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			const int2   P     = Pixel + int2(X, Y);
			const float3 Value = RgbToYCoCg(TonemapForTaa(max(SceneColor.Load(int3(P, 0)).rgb, 0.0f)));
			Mean += Value;
			MeanSquared += Value * Value;
			const float D = Depth.Load(int3(P, 0));
			if (D < Closest)
			{
				Closest      = D;
				ClosestPixel = P;
			}
		}
	}
	Mean /= 9.0f;
	const float3 Sigma  = sqrt(max(MeanSquared / 9.0f - Mean * Mean, 0.0f));
	const float3 BoxMin = Mean - VarianceGamma * Sigma;
	const float3 BoxMax = Mean + VarianceGamma * Sigma;

	if (bHistoryValid == 0)
	{
		return float4(InverseTonemapForTaa(Current), 0.0f);
	}

	// 움직임 벡터: 기하가 없으면 카메라 재투영 (먼 평면)
	float2 Motion;
	if (Closest >= 1.0f)
	{
		const float2 Ndc  = float2(Input.UV.x * 2.0f - 1.0f, 1.0f - Input.UV.y * 2.0f);
		const float4 Prev = mul(float4(Ndc, 1.0f, 1.0f), Reprojection);
		const float2 PrevUV = Prev.w > 1.0e-6f ? float2(Prev.x / Prev.w * 0.5f + 0.5f, 0.5f - Prev.y / Prev.w * 0.5f) : Input.UV;
		Motion            = Input.UV - PrevUV;
	}
	else
	{
		Motion = Velocity.Load(int3(ClosestPixel, 0));
	}
	const float2 PrevUV = Input.UV - Motion;
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return float4(InverseTonemapForTaa(Current), 0.0f); // 화면 밖에서 들어온 픽셀
	}

	const float3 HistoryYCoCg = ClipToBox(RgbToYCoCg(TonemapForTaa(SampleHistoryCatmullRom(PrevUV))), BoxMin, BoxMax);
	const float3 Clipped      = YCoCgToRgb(HistoryYCoCg);

	// 현재 비중: 반응형(파티클) + 빠른 움직임(픽셀 단위)에서 키운다
	const float Reactive    = saturate(Center.a);
	const float SpeedPixels = length(Motion / TexelSize);
	float       Weight      = lerp(CurrentWeight, ReactiveWeight, Reactive);
	Weight                  = lerp(Weight, max(Weight, 0.25f), saturate(SpeedPixels / 32.0f));

	const float3 Result = lerp(Clipped, Current, Weight);
	return float4(InverseTonemapForTaa(max(Result, 0.0f)), 0.0f);
}

#include "Common.hlsli"
#include "Fullscreen.hlsli"

// 자동 노출 (로그 휘도 히스토그램)
//   PSHistogram: 1/4 해상도 전체 화면 패스. 렌더 타깃 없이 픽셀마다 히스토그램 칸에 원자적 누적 (u0)
//   CSAverage  : 256 스레드 1그룹. 가중 평균 로그 휘도 → 이전 값과 시간 적응 → u1[0]에 기록, 히스토그램 초기화

cbuffer AutoExposureConstants : register(b0)
{
	float  MinLog2Luminance;
	float  InvLog2LuminanceRange; // 1 / (Max - Min)
	float  Log2LuminanceRange;
	float  PixelCount;            // 히스토그램에 기록한 픽셀 수
	float  DeltaSeconds;
	float  AdaptationSpeed;       // 1/초
	float2 Padding0;
};

Texture2D<float4>         SceneColor       : register(t0);
RWByteAddressBuffer       Histogram        : register(u0);
RWStructuredBuffer<float> AdaptedLuminance : register(u1);
SamplerState              LinearSampler    : register(s0);

#define HISTOGRAM_BINS 256

float Luminance(float3 Color)
{
	return dot(Color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint LuminanceToBin(float Lum)
{
	if (Lum < 1.0e-5f)
	{
		return 0;
	}
	const float LogLum = saturate((log2(Lum) - MinLog2Luminance) * InvLog2LuminanceRange);
	return (uint)(LogLum * 254.0f + 1.0f);
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

void PSHistogram(FFullscreenVSOutput Input)
{
	const float3 Color = SceneColor.SampleLevel(LinearSampler, Input.UV, 0.0f).rgb;
	const uint   Bin   = LuminanceToBin(Luminance(Color));
	Histogram.InterlockedAdd(Bin * 4, 1);
}

groupshared float GWeightedCounts[HISTOGRAM_BINS];

[numthreads(HISTOGRAM_BINS, 1, 1)]
void CSAverage(uint GroupIndex : SV_GroupIndex)
{
	const uint Count = Histogram.Load(GroupIndex * 4);
	GWeightedCounts[GroupIndex] = (float)Count * (float)GroupIndex;
	Histogram.Store(GroupIndex * 4, 0); // 다음 프레임을 위해 초기화

	GroupMemoryBarrierWithGroupSync();

	// 병렬 합산
	[unroll]
	for (uint Stride = HISTOGRAM_BINS / 2; Stride > 0; Stride >>= 1)
	{
		if (GroupIndex < Stride)
		{
			GWeightedCounts[GroupIndex] += GWeightedCounts[GroupIndex + Stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (GroupIndex == 0)
	{
		// 0번 칸(검은 픽셀)은 평균에서 제외. 이 스레드의 Count는 0번 칸 값이다
		const float Valid       = max(PixelCount - (float)Count, 1.0f);
		const float WeightedBin = GWeightedCounts[0] / Valid - 1.0f;
		const float LogLum      = (max(WeightedBin, 0.0f) / 254.0f) * Log2LuminanceRange + MinLog2Luminance;
		const float Target      = exp2(LogLum);

		// FPostProcessMath::ComputeAdaptedLuminance와 같은 식
		const float Previous = AdaptedLuminance[0];
		float       Adapted  = Target;
		if (Previous > 0.0f && !isinf(Previous) && !isnan(Previous))
		{
			Adapted = Previous + (Target - Previous) * (1.0f - exp(-max(DeltaSeconds, 0.0f) * max(AdaptationSpeed, 0.0f)));
		}
		AdaptedLuminance[0] = Adapted;
	}
}

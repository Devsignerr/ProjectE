#include "Common.hlsli"

// 2D 텍스처 밉맵 생성 컴퓨트 셰이더: 밉 N-1(SRV) → 밉 N(UAV)
//
// 필터: 목적지 텍셀 중심을 선형 샘플러로 한 번 샘플한다. 짝수 크기 소스에서는 정확히 2x2 박스 필터가 되고,
//       홀수 크기 소스에서는 보간으로 근사되어 별도의 홀수 처리 분기가 필요 없다 (UE GenerateMips와 동일 접근).
// 색공간: sRGB 포맷은 UAV를 만들 수 없으므로 리소스를 TYPELESS로 두고 SRV는 *_SRGB(읽을 때 하드웨어가 선형 변환),
//       UAV는 *_UNORM으로 만든다. 평균은 선형 공간에서 계산되고, 쓰기 직전에 다시 sRGB로 인코딩한다(bSRGB).

cbuffer MipConstants : register(b0)
{
	uint2  DstSize;      // 목적지 밉 크기
	float2 DstTexelSize; // 1 / DstSize
	uint   bSRGB;        // 1이면 쓰기 전 sRGB 인코딩
};

Texture2D<float4>   SrcMip             : register(t0);
RWTexture2D<float4> DstMip             : register(u0);
SamplerState        LinearClampSampler : register(s0);

// 선형 → sRGB 인코딩 (성분별 분기는 step/lerp로 처리: SDK 번들 DXC 1.6은 select()를 지원하지 않는다)
float3 LinearToSrgb(float3 Linear)
{
	const float3 Clamped = saturate(Linear);
	const float3 Low     = Clamped * 12.92f;
	const float3 High    = 1.055f * pow(Clamped, 1.0f / 2.4f) - 0.055f;
	const float3 UseLow  = step(Clamped, 0.0031308f); // Clamped <= 임계값이면 1
	return lerp(High, Low, UseLow);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 ThreadId : SV_DispatchThreadID)
{
	if (ThreadId.x >= DstSize.x || ThreadId.y >= DstSize.y)
	{
		return;
	}

	const float2 UV    = (float2(ThreadId.xy) + 0.5f) * DstTexelSize;
	float4       Color = SrcMip.SampleLevel(LinearClampSampler, UV, 0.0f);

	if (bSRGB != 0)
	{
		Color.rgb = LinearToSrgb(Color.rgb);
	}

	DstMip[ThreadId.xy] = Color;
}

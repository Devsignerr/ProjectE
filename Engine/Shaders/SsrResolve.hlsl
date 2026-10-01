#include "Common.hlsli"
#include "Fullscreen.hlsli"

// SSR 시간 누적 (FScreenSpaceReflections::Render 2단계, FScreenPassRootSignature). 추적은 SsrTrace.hlsl
//   확률 반사(bStochastic)일 때만: 거친 면의 광선 흔들기와 맞음/안 맞음이 픽셀·프레임마다 바뀌는 큰 노이즈는 TAA의 이웃 색 클램프가
//   걸러 내지 못해 정지 화면도 지글거린다 → 메인 패스가 읽기 전에 여기서 재투영 누적한다 (언리얼 SSR 시간 필터와 같은 역할)

SamplerState LinearSampler : register(s0);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

cbuffer SsrResolveConstants : register(b0)
{
	float2 ResolveScreenSize;
	float  ResolveCurrentWeight; // 이번 프레임 비중 (나머지는 이력)
	uint   bResolveHistoryValid;
	float  ResolveVarianceGamma; // 이력을 이웃 평균 ± Gamma·표준편차로 자른다
	float3 ResolvePadding;
};

// PSResolve 입력: t0 = 이번 추적 결과, t1 = 지난 프레임 누적 결과, t2 = 움직임 벡터 (ScreenSpace.hlsli 규약: 현재 UV − 이전 UV)
Texture2D<float4> SsrCurrent  : register(t0);
Texture2D<float4> SsrHistory  : register(t1);
Texture2D<float2> SsrVelocity : register(t2);

// 반사 색은 HDR이라 밝은 표본 하나가 평균을 지배하지 않게 신뢰도를 곱한 색을 Reinhard로 눌러 누적한다 (출력 때 되돌림)
float4 ToResolveSpace(float4 V)
{
	const float3 Premultiplied = max(V.rgb, 0.0f) * saturate(V.a);
	return float4(Premultiplied / (1.0f + dot(Premultiplied, float3(0.2126f, 0.7152f, 0.0722f))), saturate(V.a));
}

float4 FromResolveSpace(float4 V)
{
	const float3 Premultiplied = V.rgb / max(1.0f - dot(V.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-3f);
	return float4(V.a > 1.0e-4f ? Premultiplied / V.a : 0.0f, V.a);
}

float4 PSResolve(FFullscreenVSOutput Input) : SV_Target
{
	const int2 Pixel  = int2(Input.Position.xy);
	const int2 MaxPix = int2(ResolveScreenSize) - 1;

	float4 Center      = 0.0f;
	float4 Mean        = 0.0f;
	float4 MeanSquared = 0.0f;
	[unroll] for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll] for (int X = -1; X <= 1; ++X)
		{
			const float4 V = ToResolveSpace(SsrCurrent.Load(int3(clamp(Pixel + int2(X, Y), 0, MaxPix), 0)));
			Center         = (X == 0 && Y == 0) ? V : Center;
			Mean          += V;
			MeanSquared   += V * V;
		}
	}
	Mean /= 9.0f;
	const float4 Sigma = sqrt(max(MeanSquared / 9.0f - Mean * Mean, 0.0f));

	if (bResolveHistoryValid == 0)
	{
		return FromResolveSpace(Center);
	}
	const float2 PrevUV = Input.UV - SsrVelocity.Load(int3(Pixel, 0));
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return FromResolveSpace(Center);
	}
	const float4 History = clamp(ToResolveSpace(SsrHistory.SampleLevel(LinearSampler, PrevUV, 0.0f)), Mean - ResolveVarianceGamma * Sigma,
	                             Mean + ResolveVarianceGamma * Sigma);
	return FromResolveSpace(lerp(History, Center, ResolveCurrentWeight));
}

#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// SSR 시간 누적 (FScreenSpaceReflections::Render 2단계, FScreenPassRootSignature). 추적은 SsrTrace.hlsl
//   ① 확률 반사: 거친 면의 광선 흔들기와 맞음/안 맞음이 픽셀·프레임마다 바뀌는 큰 노이즈는 TAA의 이웃 색 클램프가 걸러 내지 못한다
//   ② 거울 반사: 깊이 버퍼 픽셀 단위 교차라 반사 윤곽이 계단지고 지터마다 옮겨 다닌다. 움직이면 TAA가 반사 이력을 버려 그대로 보인다
//   → 메인 패스가 읽기 전에 여기서 표면 움직임으로 재투영 누적 + 분산 클램프 (언리얼 SSR 시간 필터와 같은 역할)

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

// PSResolve 입력: t0 = 이번 추적 결과, t1 = 지난 프레임 누적 결과, t2 = 표면 움직임 벡터 (ScreenSpace.hlsli 규약: 현재 UV − 이전 UV),
//   t3 = 반사 움직임 벡터 (SsrTrace.hlsl: 반사된 가상 점 기준, 맞지 않은 픽셀은 0), t4 = 화면 법선/거칠기 (사전 패스)
Texture2D<float4> SsrCurrent       : register(t0);
Texture2D<float4> SsrHistory       : register(t1);
Texture2D<float2> SsrVelocity      : register(t2);
Texture2D<float2> SsrReflectMotion : register(t3);
Texture2D<float4> SsrSceneNormal   : register(t4);

static const float SpatialRoughnessStart = 0.05f; // 이 거칠기부터 이웃 평균을 섞기 시작 (거울은 그대로 선명)
static const float SpatialRoughnessFull  = 0.2f;  // 이 거칠기부터 3x3 평균만 (거친 반사는 원래 흐릿하다 — 확률 표본 노이즈를 프레임 안에서 먼저 줄인다)

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

	const float4 CenterNormalData = SsrSceneNormal.Load(int3(Pixel, 0));
	const float3 CenterNormal     = DecodeScreenNormal(CenterNormalData);

	float4 Center      = 0.0f;
	float4 Mean        = 0.0f;
	float4 MeanSquared = 0.0f;
	float4 Filtered    = 0.0f; // 같은 면(법선이 비슷한) 이웃만의 평균 — 다른 물체 경계로 번지지 않게
	float  FilterSum   = 0.0f;
	[unroll] for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll] for (int X = -1; X <= 1; ++X)
		{
			const int2   Tap = clamp(Pixel + int2(X, Y), 0, MaxPix);
			const float4 V   = ToResolveSpace(SsrCurrent.Load(int3(Tap, 0)));
			Center           = (X == 0 && Y == 0) ? V : Center;
			Mean            += V;
			MeanSquared     += V * V;
			const float  W   = dot(DecodeScreenNormal(SsrSceneNormal.Load(int3(Tap, 0))), CenterNormal) > 0.9f ? 1.0f : 0.0f;
			Filtered        += V * W;
			FilterSum       += W;
		}
	}
	Mean /= 9.0f;
	const float4 Sigma = sqrt(max(MeanSquared / 9.0f - Mean * Mean, 0.0f));
	// 거친 반사는 프레임 안에서 먼저 이웃 평균 (중심은 항상 자기 자신과 같은 면이라 FilterSum >= 1)
	const float SpatialWeight = saturate((DecodeScreenRoughness(CenterNormalData) - SpatialRoughnessStart) / (SpatialRoughnessFull - SpatialRoughnessStart));
	Center                    = lerp(Center, Filtered / max(FilterSum, 1.0f), SpatialWeight);

	if (bResolveHistoryValid == 0)
	{
		return FromResolveSpace(Center);
	}
	// 반사가 맞은 픽셀은 반사된 상의 움직임으로, 아니면(캡처/하늘로 대체) 표면 움직임으로 이력을 찾는다
	const float2 Motion = SsrCurrent.Load(int3(Pixel, 0)).a > 0.0f ? SsrReflectMotion.Load(int3(Pixel, 0)) : SsrVelocity.Load(int3(Pixel, 0));
	const float2 PrevUV = Input.UV - Motion;
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return FromResolveSpace(Center);
	}
	const float4 History = clamp(ToResolveSpace(SsrHistory.SampleLevel(LinearSampler, PrevUV, 0.0f)), Mean - ResolveVarianceGamma * Sigma,
	                             Mean + ResolveVarianceGamma * Sigma);
	return FromResolveSpace(lerp(History, Center, ResolveCurrentWeight));
}

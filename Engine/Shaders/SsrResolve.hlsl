#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// SSR 거칠기 흐림 + 시간 누적 (FScreenSpaceReflections::Render 2·3단계, FScreenPassRootSignature). 추적은 SsrTrace.hlsl
//   PSBlur (한 패스 원형): 추적이 낸 픽셀별 반경(GGX 반사 원뿔, ReflectionMath::ComputeSsrBlurRadiusPixels) 안의 원판을 황금각 나선 표본으로
//     가우시안 가중 평균한다. 같은 면(법선 내적 > 0.9)만 섞어 다른 물체로 번지지 않게. 거울(반경 0)은 그대로. 결정적이라 프레임마다 바뀌는 노이즈가 없다.
//     가로·세로 분리형은 쓰지 않는다 — 픽셀마다 반경이 다르고 같은 면 조건이 있으면 분리되지 않아 반사가 십자·직사각형 막대로 뭉개진다
//   PSResolve: 반사 움직임(가상 점) 재투영 누적 + 분산 클램프 — 깊이 버퍼 픽셀 단위 교차의 반사 윤곽 계단을 지터로 평균한다
//     (표면 움직임으로 찾으면 카메라가 움직일 때 반사 내용이 어긋나 클램프가 이력을 버림)

cbuffer SsrResolveConstants : register(b0)
{
	float2 ResolveScreenSize;
	float  ResolveCurrentWeight; // 이번 프레임 비중 (나머지는 이력)
	uint   bResolveHistoryValid;
	float  ResolveVarianceGamma; // 이력을 이웃 평균 ± Gamma·표준편차로 자른다
	float3 ResolvePadding;
};

// 입력 (패스별로 쓰는 것만 바인딩): t0 = 입력 반사 (rgb, 신뢰도), t1 = 지난 프레임 누적 결과, t2 = 표면 움직임 벡터 (현재 UV − 이전 UV),
//   t3 = 추적 2번째 출력 (xy = 반사 움직임 벡터 — 맞지 않은 픽셀은 0, z = 흐림 반경 픽셀), t4 = 화면 법선/거칠기 (사전 패스)
Texture2D<float4> SsrCurrent       : register(t0);
Texture2D<float4> SsrHistory       : register(t1);
Texture2D<float2> SsrVelocity      : register(t2);
Texture2D<float4> SsrTraceExtra    : register(t3);
Texture2D<float4> SsrSceneNormal   : register(t4);
SamplerState      LinearSampler    : register(s0);

static const int   BlurTaps            = 48;    // 원판 표본 수 (황금각 나선)
static const float SameSurfaceMinDot   = 0.9f;

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

// 신뢰도를 곱한 색 (가장자리 신뢰도가 섞일 때 색이 번지지 않게 곱한 채로 흐리고 누적한다)
float4 Premultiply(float4 V)
{
	return float4(max(V.rgb, 0.0f) * saturate(V.a), saturate(V.a));
}

float4 Unpremultiply(float4 V)
{
	return float4(V.a > 1.0e-4f ? V.rgb / V.a : 0.0f, V.a);
}

// 누적은 HDR 밝은 표본 하나가 평균을 지배하지 않게 Reinhard로 눌러서 (출력 때 되돌림)
float4 ToResolveSpace(float4 Premultiplied)
{
	return float4(Premultiplied.rgb / (1.0f + dot(Premultiplied.rgb, float3(0.2126f, 0.7152f, 0.0722f))), Premultiplied.a);
}

float4 FromResolveSpace(float4 V)
{
	return float4(V.rgb / max(1.0f - dot(V.rgb, float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-3f), V.a);
}

float4 PSBlur(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel  = int2(Input.Position.xy);
	const int2   MaxPix = int2(ResolveScreenSize) - 1;
	const float  Radius = SsrTraceExtra.Load(int3(Pixel, 0)).z;
	const float4 Center = Premultiply(SsrCurrent.Load(int3(Pixel, 0)));
	if (Radius < 0.5f)
	{
		return Unpremultiply(Center); // 거울
	}
	const float3 CenterNormal = DecodeScreenNormal(SsrSceneNormal.Load(int3(Pixel, 0)));
	float4       Sum          = Center;
	float        WeightSum    = 1.0f;
	[loop] for (int Index = 0; Index < BlurTaps; ++Index)
	{
		// 황금각 나선: 원판 면적을 고르게 덮는다 (반경 ∝ sqrt)
		const float  T      = ((float)Index + 0.5f) / (float)BlurTaps;
		const float  Angle  = (float)Index * 2.39996323f;
		const float2 Offset = Radius * sqrt(T) * float2(cos(Angle), sin(Angle));
		const int2   Tap    = clamp(int2(round(float2(Pixel) + Offset)), 0, MaxPix);
		if (dot(DecodeScreenNormal(SsrSceneNormal.Load(int3(Tap, 0))), CenterNormal) > SameSurfaceMinDot)
		{
			const float W = exp(-2.0f * T); // 가우시안 (거리² / 반경² = T)
			Sum          += Premultiply(SsrCurrent.Load(int3(Tap, 0))) * W;
			WeightSum    += W;
		}
	}
	return Unpremultiply(Sum / WeightSum);
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
			const float4 V = ToResolveSpace(Premultiply(SsrCurrent.Load(int3(clamp(Pixel + int2(X, Y), 0, MaxPix), 0))));
			Center         = (X == 0 && Y == 0) ? V : Center;
			Mean          += V;
			MeanSquared   += V * V;
		}
	}
	Mean /= 9.0f;
	const float4 Sigma = sqrt(max(MeanSquared / 9.0f - Mean * Mean, 0.0f));

	if (bResolveHistoryValid == 0)
	{
		return Unpremultiply(FromResolveSpace(Center));
	}
	// 반사가 맞은 픽셀은 반사된 상의 움직임으로, 아니면(캡처/하늘로 대체) 표면 움직임으로 이력을 찾는다
	const float2 ReflectMotion = SsrTraceExtra.Load(int3(Pixel, 0)).xy;
	const float2 Motion        = Center.a > 0.0f ? ReflectMotion : SsrVelocity.Load(int3(Pixel, 0));
	const float2 PrevUV        = Input.UV - Motion;
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return Unpremultiply(FromResolveSpace(Center));
	}
	const float4 History = clamp(ToResolveSpace(Premultiply(SsrHistory.SampleLevel(LinearSampler, PrevUV, 0.0f))), Mean - ResolveVarianceGamma * Sigma,
	                             Mean + ResolveVarianceGamma * Sigma);
	return Unpremultiply(FromResolveSpace(lerp(History, Center, ResolveCurrentWeight)));
}

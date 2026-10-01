#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// 화면 공간 버퍼 확인용 (FSceneRenderer::DebugView, --debug-view). 톤매핑 대신 출력에 그린다

cbuffer DebugConstants : register(b0)
{
	uint  DebugMode;     // 1 = 법선, 2 = 움직임 벡터, 3 = 깊이, 4 = 단일 채널(AO 등)
	float VelocityScale; // 움직임 벡터 배율 (UV → 표시)
	float2 DebugPadding;
};

Texture2D<float4> Source       : register(t0);
SamplerState      PointSampler : register(s1);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSMain(FFullscreenVSOutput Input) : SV_Target
{
	const float4 Value = Source.SampleLevel(PointSampler, Input.UV, 0.0f);
	float3       Color = 0.0f;
	if (DebugMode == 1)
	{
		Color = DecodeScreenNormal(Value) * 0.5f + 0.5f;
	}
	else if (DebugMode == 2)
	{
		// 오른쪽 = 빨강, 아래 = 초록, 반대 방향은 파랑 섞음
		const float2 V = Value.xy * VelocityScale;
		Color          = float3(saturate(V.x), saturate(V.y), saturate(-V.x) + saturate(-V.y));
	}
	else if (DebugMode == 3)
	{
		Color = pow(saturate(1.0f - Value.r), 0.25f); // 가까울수록 밝게 (리버스 아님: 1 = 먼 평면)
	}
	else if (DebugMode == 5)
	{
		Color = sqrt(saturate(Value.rgb * Value.a)); // SSR 색 × 신뢰도 (아래 제곱과 상쇄해 그대로 보이게)
	}
	else
	{
		Color = Value.rrr;
	}
	// 출력 RTV가 sRGB이므로 선형으로 써도 표시가 맞도록 제곱으로 되돌린다
	return float4(Color * Color, 1.0f);
}

#include "Common.hlsli"
#include "Fullscreen.hlsli"
#include "Fog.hlsli" // b0 / t1 / s0

// 불투명 메시 + 하늘에 안개 적용 (FFogRenderer, 메인 패스 뒤·파티클 전). 블렌드 EBlendMode::Premultiplied:
//   색 = 출력 rgb + 원래 × (1 - 출력 a), 알파(TAA 반응형 마스크)는 그대로. 출력 = (더할 산란, 1 - 투과율)

Texture2D<float> SceneDepth : register(t0);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSMain(FFullscreenVSOutput Input) : SV_Target
{
	const float  Depth = SceneDepth.Load(int3(int2(Input.Position.xy), 0));
	const float2 Ndc   = float2(Input.UV.x * 2.0f - 1.0f, 1.0f - Input.UV.y * 2.0f);
	float3       WorldPosition;
	if (Depth >= 1.0f)
	{
		// 하늘: 먼 평면 방향으로 SkyDistance만큼 (위로 향한 광선은 높이 감쇠로 옅어진다)
		const float4 Far = mul(float4(Ndc, 1.0f, 1.0f), FogInvViewProjection);
		WorldPosition    = FogCameraPosition + normalize(Far.xyz / Far.w - FogCameraPosition) * FogSkyDistance;
	}
	else
	{
		const float4 World = mul(float4(Ndc, Depth, 1.0f), FogInvViewProjection);
		WorldPosition      = World.xyz / World.w;
	}
	const float4 Fog = EvaluateFog(WorldPosition);
	return float4(Fog.rgb, 1.0f - Fog.a);
}

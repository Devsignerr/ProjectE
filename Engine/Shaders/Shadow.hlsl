#include "Common.hlsli"

// 섀도우 맵 깊이 패스 (방향광 캐스케이드). 픽셀 셰이더 없음

cbuffer ShadowPassConstants : register(b0)
{
	float4x4 WorldLightViewProjection;
};

float4 ShadowVS(float3 Position : POSITION) : SV_Position
{
	return mul(float4(Position, 1.0f), WorldLightViewProjection);
}

#include "Common.hlsli"
#include "SkinnedMesh.hlsli"

// 섀도우 맵 깊이 패스 (방향광 캐스케이드). 픽셀 셰이더 없음

cbuffer ShadowPassConstants : register(b0)
{
	float4x4 WorldLightViewProjection;
};

float4 ShadowVS(float3 Position : POSITION) : SV_Position
{
	return mul(float4(Position, 1.0f), WorldLightViewProjection);
}

// 스킨 메시 캐스터: 팔레트로 월드 공간 변환 후 WorldLightViewProjection(= 라이트 뷰-투영, World 항등)
float4 ShadowSkinnedVS(float3 Position : POSITION, uint4 Joints : BLENDINDICES, float4 Weights : BLENDWEIGHT) : SV_Position
{
	const float4 WorldPosition = mul(float4(Position, 1.0f), ComputeSkinMatrix(Joints, Weights));
	return mul(WorldPosition, WorldLightViewProjection);
}

#include "Common.hlsli"

// Phase 1: NDC 좌표를 그대로 출력하는 첫 삼각형

struct FVertexInput
{
	float3 Position : POSITION;
	float4 Color    : COLOR;
};

struct FPixelInput
{
	float4 Position : SV_Position;
	float4 Color    : COLOR;
};

FPixelInput VSMain(FVertexInput Input)
{
	FPixelInput Output;
	Output.Position = float4(Input.Position, 1.0f);
	Output.Color    = Input.Color;
	return Output;
}

float4 PSMain(FPixelInput Input) : SV_Target
{
	return Input.Color;
}

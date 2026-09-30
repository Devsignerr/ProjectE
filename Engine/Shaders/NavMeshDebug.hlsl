#include "Common.hlsli"

// 에디터 내비메시/경로 디버그 표시 (단위 cm). 씬 깊이와 비교만 하고(쓰기 없음) 알파 블렌드로 겹쳐 그린다.
// 정점은 CPU가 매 프레임 동적 업로드 버퍼에 올린다 (면 = 삼각형 목록, 테두리/경로 = 선 목록)

cbuffer NavDebugConstants : register(b0)
{
	float4x4 ViewProjection;
};

struct FNavDebugVSInput
{
	float3 Position : POSITION;
	float4 Color    : COLOR;
};

struct FNavDebugVSOutput
{
	float4 Position : SV_Position;
	float4 Color    : COLOR;
};

FNavDebugVSOutput NavDebugVS(FNavDebugVSInput Input)
{
	FNavDebugVSOutput Output;
	Output.Position = mul(float4(Input.Position, 1.0f), ViewProjection);
	Output.Color    = Input.Color;
	return Output;
}

float4 NavDebugPS(FNavDebugVSOutput Input) : SV_Target
{
	return Input.Color;
}

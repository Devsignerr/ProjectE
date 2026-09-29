#ifndef E_FULLSCREEN_HLSLI
#define E_FULLSCREEN_HLSLI

// 정점 버퍼 없이 SV_VertexID로 화면을 덮는 삼각형 1개 (DrawInstanced(3, 1, 0, 0))
// UV 원점은 왼쪽 위 (텍스처 좌표 규약과 동일)

struct FFullscreenVSOutput
{
	float4 Position : SV_Position;
	float2 UV       : TEXCOORD0;
};

FFullscreenVSOutput FullscreenVS(uint VertexId : SV_VertexID)
{
	FFullscreenVSOutput Output;
	Output.UV       = float2((VertexId << 1) & 2, VertexId & 2);
	Output.Position = float4(Output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return Output;
}

#endif // E_FULLSCREEN_HLSLI

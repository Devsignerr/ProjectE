cbuffer SkyConstants : register(b0)
{
	float3 Forward; float TanHalfFov;
	float3 Right; float Aspect;
	float3 Up; float Intensity;
};

TextureCube<float4> Sky : register(t0);
SamplerState LinearSampler : register(s0);

struct FOutput
{
	float4 Position : SV_Position;
	float2 Ndc : TEXCOORD0;
};

FOutput VSMain(uint Id : SV_VertexID)
{
	FOutput Out;
	Out.Ndc = float2((Id << 1) & 2, Id & 2) * 2 - 1;
	Out.Position = float4(Out.Ndc, 1, 1);
	return Out;
}

float4 PSMain(FOutput In) : SV_Target0
{
	float3 Direction = normalize(
		Forward
		+ Right * In.Ndc.x * TanHalfFov * Aspect
		+ Up * In.Ndc.y * TanHalfFov);

	return float4(
		Sky.SampleLevel(LinearSampler, Direction, 0).rgb * Intensity, 0); // 알파 0 = TAA 반응형 마스크 없음
}

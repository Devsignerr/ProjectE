#include "Common.hlsli"
#include "SkinnedMesh.hlsli"
#include "MeshInstance.hlsli"

// 섀도우 맵 깊이 패스 (방향광 캐스케이드 / 로컬 라이트 그림자 장). 불투명은 픽셀 셰이더 없음,
// Masked 머티리얼은 ShadowMasked*VS + ShadowMaskedPS (베이스 컬러 알파 < 컷오프면 버림 — Mesh.hlsl PSMainMasked와 같은 식)

// 루트 상수 17개: 라이트 뷰-투영 + 묶음의 인스턴스 번호 시작 위치
cbuffer ShadowPassConstants : register(b0)
{
	float4x4 LightViewProjection;
	uint     InstanceOffset;
};

// 정적 메시 캐스터 (인스턴싱)
float4 ShadowVS(float3 Position : POSITION, uint InstanceId : SV_InstanceID) : SV_Position
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	return mul(mul(float4(Position, 1.0f), Instance.World), LightViewProjection);
}

// 스킨 메시 캐스터 (인스턴싱): 인스턴스 팔레트로 월드 공간 변환 후 라이트 뷰-투영
float4 ShadowSkinnedVS(float3 Position : POSITION, uint4 Joints : BLENDINDICES, float4 Weights : BLENDWEIGHT, uint InstanceId : SV_InstanceID) : SV_Position
{
	const FInstanceData Instance      = LoadInstance(InstanceOffset, InstanceId);
	const float4        WorldPosition = mul(float4(Position, 1.0f), ComputeSkinMatrix(Instance.BoneOffset, Joints, Weights));
	return mul(WorldPosition, LightViewProjection);
}

// ---- Masked (알파 테스트) 캐스터: 루트 상수 b1 + 머티리얼 텍스처 테이블 첫 칸(t0 = 베이스 컬러)
cbuffer ShadowMaskConstants : register(b1)
{
	float MaskBaseAlpha;   // 머티리얼 BaseColorFactor.a
	float MaskAlphaCutoff;
};
Texture2D    MaskBaseColorTexture : register(t0);
SamplerState MaskSampler          : register(s0);

struct FShadowMaskedOutput
{
	float4 Position : SV_Position;
	float2 UV       : TEXCOORD0;
	float  Alpha    : TEXCOORD1; // 정점 색 알파
};

FShadowMaskedOutput ShadowMaskedVS(float3 Position : POSITION, float2 UV : TEXCOORD0, float4 Color : COLOR, uint InstanceId : SV_InstanceID)
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	FShadowMaskedOutput Output;
	Output.Position = mul(mul(float4(Position, 1.0f), Instance.World), LightViewProjection);
	Output.UV       = UV;
	Output.Alpha    = Color.a;
	return Output;
}

FShadowMaskedOutput ShadowSkinnedMaskedVS(float3 Position : POSITION, float2 UV : TEXCOORD0, float4 Color : COLOR, uint4 Joints : BLENDINDICES,
                                          float4 Weights : BLENDWEIGHT, uint InstanceId : SV_InstanceID)
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	FShadowMaskedOutput Output;
	Output.Position = mul(mul(float4(Position, 1.0f), ComputeSkinMatrix(Instance.BoneOffset, Joints, Weights)), LightViewProjection);
	Output.UV       = UV;
	Output.Alpha    = Color.a;
	return Output;
}

void ShadowMaskedPS(FShadowMaskedOutput Input)
{
	clip(MaskBaseColorTexture.Sample(MaskSampler, Input.UV).a * Input.Alpha * MaskBaseAlpha - MaskAlphaCutoff);
}

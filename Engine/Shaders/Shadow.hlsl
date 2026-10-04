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

#ifdef E_SKIN_CACHE
// 스킨 메시 캐스터 (스킨 캐시, Renderer/SkinCache.h): 이번 프레임 스키닝된 월드 위치 (팔레트 경로와 같은 값)
float4 ShadowSkinnedVS(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID) : SV_Position
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	return mul(LoadSkinCachePosition(Instance.SkinCacheVertex, Instance.SkinCachePrevIndex, VertexId), LightViewProjection);
}
#else
// 스킨 메시 캐스터 (인스턴싱): 인스턴스 팔레트로 월드 공간 변환 후 라이트 뷰-투영
float4 ShadowSkinnedVS(float3 Position : POSITION, uint4 Joints : BLENDINDICES, float4 Weights : BLENDWEIGHT, uint InstanceId : SV_InstanceID) : SV_Position
{
	const FInstanceData Instance      = LoadInstance(InstanceOffset, InstanceId);
	const float4        WorldPosition = mul(float4(Position, 1.0f), ComputeSkinMatrix(Instance.BoneOffset, Joints, Weights));
	return mul(WorldPosition, LightViewProjection);
}
#endif

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

#ifdef E_SKIN_CACHE
FShadowMaskedOutput ShadowSkinnedMaskedVS(float2 UV : TEXCOORD0, float4 Color : COLOR, uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	FShadowMaskedOutput Output;
	Output.Position = mul(LoadSkinCachePosition(Instance.SkinCacheVertex, Instance.SkinCachePrevIndex, VertexId), LightViewProjection);
	Output.UV       = UV;
	Output.Alpha    = Color.a;
	return Output;
}
#else
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
#endif

void ShadowMaskedPS(FShadowMaskedOutput Input)
{
	clip(MaskBaseColorTexture.Sample(MaskSampler, Input.UV).a * Input.Alpha * MaskBaseAlpha - MaskAlphaCutoff);
}

// ---- 그래프 머티리얼 Masked 캐스터 (Phase 49 사이드): 생성 함수 EvaluateMaterial의 OpacityMask로 자른다 (Mesh.hlsl PSMainMasked와 같은 식).
// 정점 셰이더는 머티리얼과 무관(Shaders.json), 픽셀 셰이더는 머티리얼마다 E_MATERIAL_GRAPH + 가상 포함 파일로 컴파일한다.
// 루트: b2 머티리얼 상수(헤더 + 파라미터), 공간 2 t0~ 머티리얼 텍스처, s0 반복 / s1 클램프. 시선 방향은 없으므로 CameraVector = 법선
struct FShadowMaterialOutput
{
	float4 Position      : SV_Position;
	float3 WorldPosition : POSITION0;
	float3 WorldNormal   : NORMAL;
	float4 WorldTangent  : TANGENT;
	float2 UV            : TEXCOORD0;
	float4 Color         : COLOR;
};

FShadowMaterialOutput MakeShadowMaterialOutput(float4 WorldPosition, float3 WorldNormal, float3x3 World3, float4 Tangent, float2 UV, float4 Color)
{
	FShadowMaterialOutput Output;
	Output.Position      = mul(WorldPosition, LightViewProjection);
	Output.WorldPosition = WorldPosition.xyz;
	Output.WorldNormal   = normalize(WorldNormal);
	const float Handedness = determinant(World3) < 0.0f ? -1.0f : 1.0f;
	Output.WorldTangent  = float4(normalize(mul(Tangent.xyz, World3)), Tangent.w * Handedness);
	Output.UV            = UV;
	Output.Color         = Color;
	return Output;
}

FShadowMaterialOutput ShadowMaterialVS(float3 Position : POSITION, float3 Normal : NORMAL, float2 UV : TEXCOORD0, float4 Color : COLOR, float4 Tangent : TANGENT,
                                       uint InstanceId : SV_InstanceID)
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	return MakeShadowMaterialOutput(mul(float4(Position, 1.0f), Instance.World), mul(Normal, GetNormalMatrix(Instance)), (float3x3)Instance.World, Tangent, UV,
	                                Color);
}

#ifdef E_SKIN_CACHE
// 스킨 캐시: 법선·탄젠트는 이미 월드 공간 (정규화, 탄젠트 w = 반사 반영 부호)
FShadowMaterialOutput ShadowMaterialSkinnedVS(float2 UV : TEXCOORD0, float4 Color : COLOR, uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	const FInstanceData    Instance = LoadInstance(InstanceOffset, InstanceId);
	const FSkinCacheVertex Vertex   = LoadSkinCacheVertex(Instance.SkinCacheVertex, Instance.SkinCachePrevIndex, VertexId);
	FShadowMaterialOutput  Output;
	Output.Position      = mul(Vertex.Position, LightViewProjection);
	Output.WorldPosition = Vertex.Position.xyz;
	Output.WorldNormal   = Vertex.Normal;
	Output.WorldTangent  = Vertex.Tangent;
	Output.UV            = UV;
	Output.Color         = Color;
	return Output;
}
#else
FShadowMaterialOutput ShadowMaterialSkinnedVS(float3 Position : POSITION, float3 Normal : NORMAL, float2 UV : TEXCOORD0, float4 Color : COLOR,
                                              float4 Tangent : TANGENT, uint4 Joints : BLENDINDICES, float4 Weights : BLENDWEIGHT,
                                              uint InstanceId : SV_InstanceID)
{
	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	const float4x4      Skin     = ComputeSkinMatrix(Instance.BoneOffset, Joints, Weights);
	return MakeShadowMaterialOutput(mul(float4(Position, 1.0f), Skin), mul(Normal, (float3x3)Skin), (float3x3)Skin, Tangent, UV, Color);
}
#endif

#ifdef E_MATERIAL_GRAPH
SamplerState MaterialShadowClampSampler : register(s1);
#define E_MATERIAL_SAMPLER_WRAP  MaskSampler
#define E_MATERIAL_SAMPLER_CLAMP MaterialShadowClampSampler
#include "MaterialCommon.hlsli"
#include "MaterialGraph.generated.hlsli"

void ShadowMaterialPS(FShadowMaterialOutput Input, bool bFrontFace : SV_IsFrontFace)
{
	FMaterialPixelInputs In;
	In.WorldPosition = Input.WorldPosition;
	In.WorldNormal   = normalize(Input.WorldNormal);
	In.WorldTangent  = Input.WorldTangent;
	In.UV0           = Input.UV;
	In.VertexColor   = Input.Color;
	In.CameraVector  = In.WorldNormal;
	In.PixelPosition = Input.Position.xy;
	In.bFrontFace    = bFrontFace;
	FMaterialSurface Material;
	EvaluateMaterial(In, Material);
	clip(Material.OpacityMask - E_MATERIAL_HEADER.y);
}
#endif
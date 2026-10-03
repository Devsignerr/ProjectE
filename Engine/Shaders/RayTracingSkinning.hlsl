#include "SkinnedMesh.hlsli" // t15 SkinBones (프레임 팔레트)

// 레이 트레이싱 스키닝 (Phase 50, FRayTracingScene): 스킨 메시 정점을 프레임 팔레트로 월드 공간에 변환해 FVertex(64B) 버퍼에 쓴다.
//   BLAS 갱신(refit) 입력과 히트 정점 보간(RayTracingCommon.hlsli LoadHitGeometry — 인스턴스 변환 = 항등)이 같은 버퍼를 읽는다.
//   식은 Mesh.hlsl VSSkinned와 같다 (위치/법선/탄젠트 = 스킨 행렬, 반사 행렬이면 탄젠트 w 반전)

cbuffer SkinningConstants : register(b0)
{
	uint VertexCount;
	uint BoneOffset; // 팔레트 안 첫 본 (FMeshInstance::BoneOffset)
};

ByteAddressBuffer   BaseVertices : register(t0); // FVertex: Position(0) Normal(12) UV(24) Color(32) Tangent(48)
ByteAddressBuffer   SkinVertices : register(t1); // FSkinVertex: Joints uint16 × 4 (0), Weights float4 (8) — 24B
RWByteAddressBuffer OutVertices  : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 DispatchId : SV_DispatchThreadID)
{
	const uint Vertex = DispatchId.x;
	if (Vertex >= VertexCount)
	{
		return;
	}
	const uint   Base     = Vertex * 64;
	const float3 Position = asfloat(BaseVertices.Load3(Base));
	const float3 Normal   = asfloat(BaseVertices.Load3(Base + 12));
	const uint2  UV       = BaseVertices.Load2(Base + 24);
	const uint4  Color    = BaseVertices.Load4(Base + 32);
	const float4 Tangent  = asfloat(BaseVertices.Load4(Base + 48));

	const uint   Skin    = Vertex * 24;
	const uint2  Packed  = SkinVertices.Load2(Skin);
	const uint4  Joints  = uint4(Packed.x & 0xFFFFu, Packed.x >> 16, Packed.y & 0xFFFFu, Packed.y >> 16);
	const float4 Weights = asfloat(SkinVertices.Load4(Skin + 8));

	const float4x4 SkinMatrix = ComputeSkinMatrix(BoneOffset, Joints, Weights);
	const float3x3 Skin3      = (float3x3)SkinMatrix;
	const float    Handedness = determinant(Skin3) < 0.0f ? -1.0f : 1.0f;

	OutVertices.Store3(Base, asuint(mul(float4(Position, 1.0f), SkinMatrix).xyz));
	OutVertices.Store3(Base + 12, asuint(normalize(mul(Normal, Skin3))));
	OutVertices.Store2(Base + 24, UV);
	OutVertices.Store4(Base + 32, Color);
	OutVertices.Store4(Base + 48, asuint(float4(normalize(mul(Tangent.xyz, Skin3)), Tangent.w * Handedness)));
}
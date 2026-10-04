#include "SkinnedMesh.hlsli" // t15 SkinBones (프레임 팔레트)

// 스킨 캐시 (Renderer/SkinCache.h): 같은 메시·LOD 인스턴스 묶음 하나를 디스패치 하나로 스키닝한다 (X = 정점 목록, Y = 항목).
//   영역(SkinnedMesh.hlsli, SkinCacheMath): 위치 float4 / 법선·탄젠트 float4×2 / 이전 위치 float4(이전 팔레트, w = 현재 가중치 합)
//   + bAttributes면 RT 정점 영역에 완전한 FVertex (레이 트레이싱 정점 풀로 그대로 복사된다)
//   식은 Mesh.hlsl VSSkinned(팔레트 경로)·RayTracingSkinning.hlsl과 같다 (반사 행렬이면 탄젠트 w 반전)

cbuffer SkinCacheConstants : register(b0)
{
	uint ThreadCount; // 스키닝할 정점 수 (목록 크기, 항등이면 메시 정점 수)
	uint FirstItem;   // 항목 표 안 이 디스패치의 첫 항목
	uint bUseList;    // 1 = 정점 번호를 목록(t3)에서 — 인스턴스 LOD 이상이 쓰는 정점만 (FStaticMesh::GetLodVertexList)
	uint bAttributes; // 1 = UV/색도 쓴다 (레이 트레이싱이 복사할 완전한 FVertex). 0이면 래스터가 읽는 위치/법선/탄젠트만 (쓰기 대역폭)
};

struct FSkinCacheItem
{
	uint FirstVertex;    // 영역 안 첫 정점 (캐시 정점 번호 = 첫 정점 + 메시 정점 번호)
	uint Capacity;       // 용량 C (영역 시작 — SkinnedMesh.hlsli)
	uint BoneOffset;     // 팔레트 안 첫 본
	uint PrevBoneOffset; // 이전 프레임 팔레트 (이력 없으면 BoneOffset)
	uint RtFirstVertex;  // RT 정점 영역 안 첫 정점 (bAttributes일 때)
	uint3 Padding;
};

ByteAddressBuffer                BaseVertices : register(t0); // FVertex: Position(0) Normal(12) UV(24) Color(32) Tangent(48)
ByteAddressBuffer                SkinVertices : register(t1); // FSkinVertex: Joints uint16 × 4 (0), Weights float4 (8) — 24B
StructuredBuffer<FSkinCacheItem> Items        : register(t2);
StructuredBuffer<uint>           VertexList   : register(t3);
RWByteAddressBuffer              OutVertices  : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 DispatchId : SV_DispatchThreadID)
{
	if (DispatchId.x >= ThreadCount)
	{
		return;
	}
	const uint Vertex = bUseList != 0 ? VertexList[DispatchId.x] : DispatchId.x; // 캐시 자리 = 정점 번호 그대로
	const FSkinCacheItem Item = Items[FirstItem + DispatchId.y];

	const uint   Base     = Vertex * 64;
	const float3 Position = asfloat(BaseVertices.Load3(Base));
	const float3 Normal   = asfloat(BaseVertices.Load3(Base + 12));
	const float4 Tangent  = asfloat(BaseVertices.Load4(Base + 48));

	const uint   Skin    = Vertex * 24;
	const uint2  Packed  = SkinVertices.Load2(Skin);
	const uint4  Joints  = uint4(Packed.x & 0xFFFFu, Packed.x >> 16, Packed.y & 0xFFFFu, Packed.y >> 16);
	const float4 Weights = asfloat(SkinVertices.Load4(Skin + 8));

	const float4x4 SkinMatrix    = ComputeSkinMatrix(Item.BoneOffset, Joints, Weights);
	const float4   WorldPosition = mul(float4(Position, 1.0f), SkinMatrix);
	const float3x3 Skin3         = (float3x3)SkinMatrix;
	const float    Handedness    = determinant(Skin3) < 0.0f ? -1.0f : 1.0f;

	const uint   Index   = Item.FirstVertex + Vertex;
	const float3 Normal3 = normalize(mul(Normal, Skin3));
	const float4 Tangent4 = float4(normalize(mul(Tangent.xyz, Skin3)), Tangent.w * Handedness);
	OutVertices.Store4(Index * 16, asuint(WorldPosition));
	OutVertices.Store4(Item.Capacity * 16 + Index * 32, asuint(float4(Normal3, Tangent4.w)));
	OutVertices.Store4(Item.Capacity * 16 + Index * 32 + 16, asuint(float4(Tangent4.xyz, 0.0f)));
	if (bAttributes != 0)
	{
		// 레이 트레이싱 정점 영역: FVertex 그대로 (RayTracingSkinning.hlsl과 같은 값 — RT가 정점 풀로 복사)
		const uint Out = Item.Capacity * 64 + (Item.RtFirstVertex + Vertex) * 64;
		OutVertices.Store3(Out, asuint(WorldPosition.xyz));
		OutVertices.Store3(Out + 12, asuint(Normal3));
		OutVertices.Store2(Out + 24, BaseVertices.Load2(Base + 24));
		OutVertices.Store4(Out + 32, BaseVertices.Load4(Base + 32));
		OutVertices.Store4(Out + 48, asuint(Tangent4));
	}

	float4 PrevPosition = WorldPosition;
	if (Item.PrevBoneOffset != Item.BoneOffset)
	{
		PrevPosition = mul(float4(Position, 1.0f), ComputeSkinMatrix(Item.PrevBoneOffset, Joints, Weights));
	}
	OutVertices.Store4(Item.Capacity * 48 + Index * 16, asuint(float4(PrevPosition.xyz, WorldPosition.w)));
}

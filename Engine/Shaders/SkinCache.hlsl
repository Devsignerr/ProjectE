#include "SkinnedMesh.hlsli" // t15 SkinBones (프레임 팔레트)

// 스킨 캐시 (Renderer/SkinCache.h): 같은 메시·LOD 인스턴스 묶음 하나를 디스패치 하나로 스키닝한다 (X = 정점 목록, Y = 항목).
//   현재 영역: FVertex(64B) 자리 그대로 월드 공간 — 위치/법선/탄젠트 = 스킨 행렬, UV/색은 bAttributes일 때만 복사 (레이 트레이싱 정점 풀로 그대로 복사된다)
//   이전 영역: float4 (이전 프레임 팔레트로 변환한 위치 xyz, 현재 위치 w = 가중치 합)
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
	uint FirstVertex;    // 현재 영역 첫 정점
	uint PrevIndex;      // 이전 영역 첫 정점의 16바이트 칸 번호 (버퍼 시작 기준)
	uint BoneOffset;     // 팔레트 안 첫 본
	uint PrevBoneOffset; // 이전 프레임 팔레트 (이력 없으면 BoneOffset)
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

	const uint Out = (Item.FirstVertex + Vertex) * 64;
	OutVertices.Store3(Out, asuint(WorldPosition.xyz));
	OutVertices.Store3(Out + 12, asuint(normalize(mul(Normal, Skin3))));
	if (bAttributes != 0)
	{
		const uint2 UV    = BaseVertices.Load2(Base + 24);
		const uint4 Color = BaseVertices.Load4(Base + 32);
		OutVertices.Store2(Out + 24, UV);
		OutVertices.Store4(Out + 32, Color);
	}
	OutVertices.Store4(Out + 48, asuint(float4(normalize(mul(Tangent.xyz, Skin3)), Tangent.w * Handedness)));

	float4 PrevPosition = WorldPosition;
	if (Item.PrevBoneOffset != Item.BoneOffset)
	{
		PrevPosition = mul(float4(Position, 1.0f), ComputeSkinMatrix(Item.PrevBoneOffset, Joints, Weights));
	}
	OutVertices.Store4((Item.PrevIndex + Vertex) * 16, asuint(float4(PrevPosition.xyz, WorldPosition.w)));
}

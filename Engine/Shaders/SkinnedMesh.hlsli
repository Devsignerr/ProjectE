// GPU 스키닝 공용 (루트 SRV t15 하나를 두 방식이 나눠 쓴다 — 정점 셰이더는 디파인으로 고른다)
//
// 기본 (E_SKIN_CACHE 없음): 프레임 본 팔레트 (SkinnedMeshPalette.h — 보이는 스킨 메시의 팔레트를 이어 붙인 구조화 버퍼)
//   팔레트[BoneOffset + i] = InverseBind[i] * JointWorld[i] (행벡터 규약) — 스킨 정점은 바로 월드 공간으로 변환된다
//   BoneOffset은 인스턴스 데이터(MeshInstance.hlsli FInstanceData.BoneOffset)에 있다. 조인트 번호는 SkinnedMeshData.h MaxSkinJoints 미만
//
// E_SKIN_CACHE: 스킨 캐시 (Renderer/SkinCache.h — 프레임마다 계산 셰이더 SkinCache.hlsl이 보이는 스킨 인스턴스를 한 번 스키닝한 결과)
//   용량 C 기준 영역: 위치 float4 [0,16C) / 법선·탄젠트 float4×2 [16C,48C) / 이전 위치 float4 [48C,64C) (SkinCacheMath::Get*Offset)
//   정점 번호 = 인스턴스 SkinCacheVertex + SV_VertexID (메시 그리기는 BaseVertexLocation 0). 값은 팔레트 경로 정점 셰이더와 같은 식의 결과
//   (위치 w = 가중치 합 — 팔레트 경로의 mul(float4(P, 1), Skin).w와 같다)

#ifdef E_SKIN_CACHE

ByteAddressBuffer SkinCache : register(t15);

struct FSkinCacheVertex
{
	float4 Position;     // 월드 (w = 가중치 합)
	float4 PrevPosition; // 이전 프레임 월드 (이력 없으면 Position)
	float3 Normal;       // 월드, 정규화
	float4 Tangent;      // 월드 xyz 정규화, w = 바이탄젠트 부호 (반사 행렬이면 반전됨)
};

float4 LoadSkinCachePosition(uint CacheVertex, uint Capacity, uint VertexId)
{
	return asfloat(SkinCache.Load4((CacheVertex + VertexId) * 16));
}

FSkinCacheVertex LoadSkinCacheVertex(uint CacheVertex, uint Capacity, uint VertexId)
{
	const uint   Index = CacheVertex + VertexId;
	const float4 NormalW = asfloat(SkinCache.Load4(Capacity * 16 + Index * 32));
	const float4 TangentXyz = asfloat(SkinCache.Load4(Capacity * 16 + Index * 32 + 16));
	FSkinCacheVertex Vertex;
	Vertex.Position     = asfloat(SkinCache.Load4(Index * 16));
	Vertex.PrevPosition = asfloat(SkinCache.Load4(Capacity * 48 + Index * 16));
	Vertex.Normal       = NormalW.xyz;
	Vertex.Tangent      = float4(TangentXyz.xyz, NormalW.w);
	return Vertex;
}

#else

StructuredBuffer<float4x4> SkinBones : register(t15);

float4x4 ComputeSkinMatrix(uint BoneOffset, uint4 Joints, float4 Weights)
{
	return SkinBones[BoneOffset + Joints.x] * Weights.x + SkinBones[BoneOffset + Joints.y] * Weights.y +
	       SkinBones[BoneOffset + Joints.z] * Weights.z + SkinBones[BoneOffset + Joints.w] * Weights.w;
}

#endif

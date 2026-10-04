// GPU 스키닝 공용 (루트 SRV t15 하나를 두 방식이 나눠 쓴다 — 정점 셰이더는 디파인으로 고른다)
//
// 기본 (E_SKIN_CACHE 없음): 프레임 본 팔레트 (SkinnedMeshPalette.h — 보이는 스킨 메시의 팔레트를 이어 붙인 구조화 버퍼)
//   팔레트[BoneOffset + i] = InverseBind[i] * JointWorld[i] (행벡터 규약) — 스킨 정점은 바로 월드 공간으로 변환된다
//   BoneOffset은 인스턴스 데이터(MeshInstance.hlsli FInstanceData.BoneOffset)에 있다. 조인트 번호는 SkinnedMeshData.h MaxSkinJoints 미만
//
// E_SKIN_CACHE: 스킨 캐시 (Renderer/SkinCache.h — 프레임마다 계산 셰이더 SkinCache.hlsl이 보이는 스킨 인스턴스를 한 번 스키닝한 결과)
//   [현재 정점 영역: FVertex 64B × 용량 — 월드 위치/법선/UV/색/탄젠트][이전 영역: float4 16B × 용량 — 이전 프레임 월드 위치 xyz + 위치 w]
//   인스턴스 데이터 SkinCacheVertex(현재 영역 정점 번호) / SkinCachePrevIndex(버퍼 안 16바이트 칸 번호) + SV_VertexID로 읽는다.
//   값은 팔레트 경로 정점 셰이더와 같은 식으로 계산된다 (위치 w = 가중치 합 — 팔레트 경로의 mul(float4(P, 1), Skin).w와 같다)

#ifdef E_SKIN_CACHE

ByteAddressBuffer SkinCache : register(t15);

struct FSkinCacheVertex
{
	float4 Position;     // 월드 (w = 가중치 합)
	float4 PrevPosition; // 이전 프레임 월드 (이력 없으면 Position)
	float3 Normal;       // 월드, 정규화
	float4 Tangent;      // 월드 xyz 정규화, w = 바이탄젠트 부호 (반사 행렬이면 반전됨)
};

float4 LoadSkinCachePosition(uint CacheVertex, uint PrevIndex, uint VertexId)
{
	const float3 Position = asfloat(SkinCache.Load3((CacheVertex + VertexId) * 64));
	const float  W        = asfloat(SkinCache.Load(((PrevIndex + VertexId) * 16) + 12));
	return float4(Position, W);
}

FSkinCacheVertex LoadSkinCacheVertex(uint CacheVertex, uint PrevIndex, uint VertexId)
{
	const uint   Base = (CacheVertex + VertexId) * 64;
	const float4 Prev = asfloat(SkinCache.Load4((PrevIndex + VertexId) * 16));
	FSkinCacheVertex Vertex;
	Vertex.Position     = float4(asfloat(SkinCache.Load3(Base)), Prev.w);
	Vertex.PrevPosition = Prev;
	Vertex.Normal       = asfloat(SkinCache.Load3(Base + 12));
	Vertex.Tangent      = asfloat(SkinCache.Load4(Base + 48));
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

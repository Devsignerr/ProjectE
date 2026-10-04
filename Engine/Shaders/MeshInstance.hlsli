#ifndef E_MESH_INSTANCE_HLSLI
#define E_MESH_INSTANCE_HLSLI

// 메시 GPU 인스턴싱 공용 (정적 + 스킨) (Renderer/MeshInstancing.h, ShaderTypes.h FInstanceGpuData와 1:1)
//   Instances(t13)   = 프레임 인스턴스 목록 (월드 + 법선 행렬)
//   InstanceIndices(t14) = 패스가 묶음 순서로 이어 붙인 인스턴스 번호. 묶음 하나 = [InstanceOffset, + 인스턴스 수)
//   InstanceOffset은 포함하는 셰이더의 루트 상수(b0)에 있다

struct FInstanceData
{
	float4x4 World;
	float4   NormalMatrix[3]; // (World⁻¹)ᵀ 상단 3x3 행
	uint     BoneOffset;      // 스킨 메시: 프레임 팔레트(t15, SkinnedMesh.hlsli) 안 첫 본 행렬 번호
	uint     PrevBoneOffset;  // 스킨 메시: 이전 프레임 팔레트 첫 본 (움직임 벡터, 이력 없으면 BoneOffset)
	uint     SkinCacheVertex;    // 스킨 캐시(E_SKIN_CACHE, SkinnedMesh.hlsli): 현재 정점 영역 안 첫 정점
	uint     SkinCachePrevIndex; // 스킨 캐시: 이전 위치 영역 첫 정점의 버퍼 안 16바이트 칸 번호
	float4x4 PrevWorld;       // 이전 프레임 월드 (움직임 벡터, 이력 없으면 World)
};

StructuredBuffer<FInstanceData> Instances       : register(t13);
StructuredBuffer<uint>          InstanceIndices : register(t14);

FInstanceData LoadInstance(uint Offset, uint InstanceId)
{
	return Instances[InstanceIndices[Offset + InstanceId]];
}

float3x3 GetNormalMatrix(FInstanceData Instance)
{
	return float3x3(Instance.NormalMatrix[0].xyz, Instance.NormalMatrix[1].xyz, Instance.NormalMatrix[2].xyz);
}

#endif // E_MESH_INSTANCE_HLSLI

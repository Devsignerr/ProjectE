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
	uint3    Padding;
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

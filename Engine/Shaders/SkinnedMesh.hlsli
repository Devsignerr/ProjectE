// GPU 스키닝 공용: 프레임 본 팔레트 (SkinnedMeshPalette.h — 보이는 스킨 메시의 팔레트를 이어 붙인 구조화 버퍼, 루트 SRV t15)
// 팔레트[BoneOffset + i] = InverseBind[i] * JointWorld[i] (행벡터 규약) — 스킨 정점은 바로 월드 공간으로 변환된다
// BoneOffset은 인스턴스 데이터(MeshInstance.hlsli FInstanceData.BoneOffset)에 있다. 조인트 번호는 SkinnedMeshData.h MaxSkinJoints 미만

StructuredBuffer<float4x4> SkinBones : register(t15);

float4x4 ComputeSkinMatrix(uint BoneOffset, uint4 Joints, float4 Weights)
{
	return SkinBones[BoneOffset + Joints.x] * Weights.x + SkinBones[BoneOffset + Joints.y] * Weights.y +
	       SkinBones[BoneOffset + Joints.z] * Weights.z + SkinBones[BoneOffset + Joints.w] * Weights.w;
}

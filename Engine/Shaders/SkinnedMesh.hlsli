// GPU 스키닝 공용: 본 행렬 팔레트 (SkinnedMeshData.h MaxSkinJoints와 일치, 루트 CBV b4)
// 팔레트[i] = InverseBind[i] * JointWorld[i] (행벡터 규약) — 스킨 정점은 바로 월드 공간으로 변환된다

cbuffer SkinPalette : register(b4)
{
	float4x4 SkinBones[256];
};

float4x4 ComputeSkinMatrix(uint4 Joints, float4 Weights)
{
	return SkinBones[Joints.x] * Weights.x + SkinBones[Joints.y] * Weights.y +
	       SkinBones[Joints.z] * Weights.z + SkinBones[Joints.w] * Weights.w;
}

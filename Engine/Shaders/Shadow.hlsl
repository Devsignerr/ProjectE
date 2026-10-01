#include "Common.hlsli"
#include "SkinnedMesh.hlsli"
#include "MeshInstance.hlsli"

// 섀도우 맵 깊이 패스 (방향광 캐스케이드 / 로컬 라이트 그림자 장). 픽셀 셰이더 없음

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

// 스킨 메시 캐스터: 팔레트로 월드 공간 변환 후 라이트 뷰-투영
float4 ShadowSkinnedVS(float3 Position : POSITION, uint4 Joints : BLENDINDICES, float4 Weights : BLENDWEIGHT) : SV_Position
{
	const float4 WorldPosition = mul(float4(Position, 1.0f), ComputeSkinMatrix(Joints, Weights));
	return mul(WorldPosition, LightViewProjection);
}

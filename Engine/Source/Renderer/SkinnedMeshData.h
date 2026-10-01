#pragma once

#include "Core/Math/Math.h"

#include <string>
#include <vector>

// 스킨 정점 스트림 (정점 버퍼 슬롯 1). FVertex(슬롯 0)와 같은 개수·순서.
// 정적 메시는 이 스트림이 없으므로 정적 경로의 정점 크기/대역폭은 그대로다.
struct FSkinVertex
{
	uint16   Joints[4]  = { 0, 0, 0, 0 };                 // FSkinComponent::Joints 인덱스
	FVector4 Weights    = FVector4(1.0f, 0.0f, 0.0f, 0.0f); // 합 = 1
};
static_assert(sizeof(FSkinVertex) == 24);

// 메시 하나의 최대 조인트 수 (프레임 팔레트 SkinnedMesh.hlsli SkinBones에서 인스턴스마다 이만큼까지). 초과 조인트는 임포트 시 경고 후 잘린다
inline constexpr uint32 MaxSkinJoints = 256;

// glTF 스킨 (엔진 좌표계/단위로 변환됨)
struct FModelSkin
{
	std::string             Name;
	std::vector<int32>      Joints;              // FModelData::Nodes 인덱스
	std::vector<FMatrix4x4> InverseBindMatrices; // Joints와 같은 개수
};

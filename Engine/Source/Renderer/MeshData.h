#pragma once

#include "Core/Math/Math.h"

#include <vector>

// 정적 메시 정점 (셰이더 입력 레이아웃은 FStaticMesh::GetInputLayout 참조)
struct FVertex
{
	FVector3 Position;
	FVector3 Normal;
	FVector2 UV;
	FVector4 Color = FVector4::OneVector;
};

// CPU 측 메시 데이터. 인덱스는 삼각형 리스트, 앞면은 시계 방향(CW).
struct FMeshData
{
	std::vector<FVertex> Vertices;
	std::vector<uint32>  Indices;
};

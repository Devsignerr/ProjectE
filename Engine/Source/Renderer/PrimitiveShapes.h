#pragma once

#include "Renderer/MeshData.h"

// 절차적 기본 도형 생성
struct FPrimitiveShapes
{
	// 원점 중심, 한 변 Size인 정육면체. 면마다 독립 정점(24개)으로 법선/UV가 올바르다.
	static FMeshData MakeCube(float Size = 1.0f, const FVector4& Color = FVector4::OneVector);
};

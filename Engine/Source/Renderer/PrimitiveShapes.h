#pragma once

#include "Renderer/MeshData.h"

// 절차적 기본 도형 생성
struct FPrimitiveShapes
{
	// 원점 중심, 한 변 Size인 정육면체. 면마다 독립 정점(24개)으로 법선/UV가 올바르다.
	static FMeshData MakeCube(float Size = 1.0f, const FVector4& Color = FVector4::OneVector);
	// 원점 중심 UV 구 (+Z 극). 앞면 CW 규약(Cross(P1-P0, P2-P0)·Normal > 0)을 삼각형마다 맞춘다
	static FMeshData MakeSphere(float Radius = 0.5f, uint32 Segments = 32, uint32 Rings = 16, const FVector4& Color = FVector4::OneVector);
};

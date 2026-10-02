#pragma once

#include "Renderer/MeshData.h"

// 절차적 기본 도형 생성
struct FPrimitiveShapes
{
	// 원점 중심, 한 변 Size인 정육면체. 면마다 독립 정점(24개)으로 법선/UV가 올바르다.
	static FMeshData MakeCube(float Size = 1.0f, const FVector4& Color = FVector4::OneVector);
	// 원점 중심, 한 변 Size인 사각형 한 장 (XY 평면, 법선 +Z, UV는 +X 위쪽). 뒷면은 없다 — 양면 머티리얼(TwoSided)과 함께 쓴다
	static FMeshData MakePlane(float Size = 1.0f, const FVector4& Color = FVector4::OneVector);
	// 원점 중심 UV 구 (+Z 극). 앞면 CW 규약(Cross(P1-P0, P2-P0)·Normal > 0)을 삼각형마다 맞춘다
	static FMeshData MakeSphere(float Radius = 0.5f, uint32 Segments = 32, uint32 Rings = 16, const FVector4& Color = FVector4::OneVector);
	// Z축 캡슐: 반지름 Radius, 원기둥 부분 절반 높이 HalfHeight (전체 높이 = 2 * (HalfHeight + Radius)) — FCapsuleColliderComponent와 같은 정의
	static FMeshData MakeCapsule(float Radius = 0.5f, float HalfHeight = 0.5f, uint32 Segments = 32, uint32 HemisphereRings = 8, const FVector4& Color = FVector4::OneVector);
};

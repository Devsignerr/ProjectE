#pragma once

#include "Core/Math/Math.h"

#include <vector>

// 정적 메시 정점 (셰이더 입력 레이아웃은 FStaticMesh::GetInputLayout 참조)
// 탄젠트 규약: Tangent.xyz = UV의 +U 방향, 바이탄젠트 B = Cross(Normal, Tangent.xyz) * Tangent.w 는
// 노멀 맵의 +Y(초록)가 가리키는 방향 = UV의 -V 방향(UV 원점이 왼쪽 위이므로 "위쪽")
struct FVertex
{
	FVector3 Position;
	FVector3 Normal;
	FVector2 UV;
	FVector4 Color   = FVector4::OneVector;
	FVector4 Tangent = FVector4(1.0f, 0.0f, 0.0f, 1.0f);
};

// 단순화 LOD 하나 (LOD1 이상). 정점은 LOD0과 공유하고 인덱스만 따로 (Renderer/MeshSimplifier)
struct FMeshLod
{
	std::vector<uint32> Indices;
	float               ScreenSize = 0.0f; // 화면 크기가 이보다 작으면 이 LOD (LodMath)
};

// CPU 측 메시 데이터. 인덱스는 삼각형 리스트, 앞면은 시계 방향(CW).
struct FMeshData
{
	std::vector<FVertex> Vertices;
	std::vector<uint32>  Indices; // LOD0
	std::vector<FMeshLod> Lods;   // LOD1.. (비어 있으면 LOD0만)

	// 위치/UV/법선으로 탄젠트를 계산해 모든 정점에 채운다 (삼각형 단위 계산 → 정점 누적 → 그람-슈미트).
	// UV가 퇴화한 정점은 법선에 수직인 임의 탄젠트를 쓴다.
	void ComputeTangents();
};

// 탄젠트 단위 계산 (테스트용 공개): 삼각형의 dP/du, 그리고 -dP/dv 방향(노멀 맵 +Y)
struct FTangentBasis
{
	FVector3 Tangent;   // dP/du
	FVector3 Bitangent; // -dP/dv
	bool     bValid = false;
};
FTangentBasis ComputeTriangleTangentBasis(const FVector3& P0, const FVector3& P1, const FVector3& P2,
                                          const FVector2& UV0, const FVector2& UV1, const FVector2& UV2);

// 법선 N과 누적 탄젠트/바이탄젠트로 (정규 직교 탄젠트, 부호) 계산
FVector4 OrthonormalizeTangent(const FVector3& Normal, const FVector3& Tangent, const FVector3& Bitangent);

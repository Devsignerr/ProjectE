#pragma once

#include "Renderer/MeshData.h"

#include <vector>

// 메시 단순화 (LOD 생성, 순수 로직 — RendererTests의 LodTests)
//   이차 오차(QEM) 반-모서리 붕괴(half-edge collapse): 위치가 같은 정점을 하나의 "위치"로 묶고, 위치 P를 이웃 위치 Q로 옮기는
//   붕괴를 오차(면 평면 이차식 합을 Q에서 평가)가 작은 순으로 반복한다. 정점은 기존 정점만 쓰므로(새 정점 없음) LOD는 정점 버퍼를
//   LOD0과 공유하고 인덱스만 새로 만든다.
//   속성 이음매(UV/법선이 갈라지는 곳) 보존: P의 속성 정점 p마다, p와 한 삼각형에 함께 있는 Q의 속성 정점 q가 있어야 붕괴할 수 있고
//   p는 그 q로 바뀐다 → 이음매를 가로질러 UV가 섞이지 않고, 이음매·각진 모서리는 그 선을 따라서만 줄어든다 (꼭짓점은 사실상 고정).
//   열린 경계는 경계 모서리를 따라서만, 비다양체 모서리(삼각형 3개 이상)의 위치는 고정. 붕괴로 뒤집히는(법선이 반대가 되는) 삼각형이
//   생기면 그 붕괴는 하지 않는다. 와인딩(CW)은 유지된다.
namespace MeshSimplifier
{
	// 삼각형 수가 TargetTriangles 이하가 될 때까지 (또는 더 붕괴할 수 없을 때까지) 단순화한 인덱스
	std::vector<uint32> SimplifyToTriangleCount(const std::vector<FVertex>& Vertices, const std::vector<uint32>& Indices, uint32 TargetTriangles);

	// LOD1..(LodCount-1)을 만들어 Mesh.Lods를 채운다 (LodMath 기본 비율·화면 크기, 앞 LOD에서 이어서 단순화).
	// 원본이 MinTriangles 미만이거나 직전 LOD보다 충분히 줄지 않으면(85% 초과) 거기서 멈춘다. LodCount ≤ 1이면 Lods를 비운다
	constexpr uint32 MinTriangles = 64;
	void GenerateLods(FMeshData& Mesh, uint32 LodCount);

	// LOD0을 TargetTriangles 이하로 단순화하고 쓰지 않게 된 정점을 버린다 (임포트 설정 MaxTriangles — 스캔 에셋용). 이미 작으면 그대로
	void SimplifyBase(FMeshData& Mesh, uint32 TargetTriangles);
	// 쓰는 정점만 남기고 인덱스를 다시 매긴다 (순서 = 처음 쓰인 순서). LOD 인덱스도 함께
	void CompactVertices(FMeshData& Mesh);

	// ---- 잎 솎아내기 (마스크 컷아웃 잎처럼 서로 떨어진 작은 조각이 많은 메시 — QEM은 조각 사이를 붕괴하지 못한다)
	//   조각(섬) = 위치가 같은 정점으로 이어진 삼각형 묶음. 섬마다 고정 해시 순으로 KeepRatio만큼 남기고, 남은 섬은 무게중심 기준으로
	//   1/sqrt(KeepRatio)배(최대 MaxThinningScale) 키워 덮는 면적을 유지한다. 결정적(입력 순서만 사용).
	constexpr uint32 MinIslands         = 256; // 섬이 이보다 적으면 잎 메시로 보지 않는다
	constexpr uint32 MaxIslandTriangles = 96;  // 섬 하나의 평균 삼각형이 이보다 많으면 잎 메시로 보지 않는다
	constexpr float  MaxThinningScale   = 3.0f;
	bool IsIslandMesh(const FMeshData& Mesh);
	// 삼각형마다 섬 번호(처음 나온 순서)를 채우고 섬 수를 돌려준다
	uint32 FindIslands(const FMeshData& Mesh, std::vector<uint32>& OutTriangleIsland);
	// LOD0 자체를 TargetTriangles 근처로 솎아 낸다 (정점·인덱스 교체, Lods 비움)
	void ThinBase(FMeshData& Mesh, uint32 TargetTriangles);
	// LOD1..을 솎아내기로 만든다 — 키운 정점은 정점 버퍼 뒤에 덧붙인다 (LOD0 인덱스·정점은 그대로)
	void GenerateIslandLods(FMeshData& Mesh, uint32 LodCount);
} // namespace MeshSimplifier

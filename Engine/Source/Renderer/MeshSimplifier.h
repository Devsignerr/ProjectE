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
} // namespace MeshSimplifier

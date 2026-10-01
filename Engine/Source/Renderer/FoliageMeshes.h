#pragma once

#include "Renderer/MeshData.h"

#include <string_view>

// 내장 폴리지 절차 메시 ("foliage:<이름>"): grass(풀 덤불, 양면 잎), bush(덤불), tree(활엽수), pine(침엽수), rock(바위).
// 원점 = 밑동, +Z 위, cm. 색은 정점 색(선형) — 기본 머티리얼(흰색)과 곱해진다. 닫힌 메시는 LOD를 만든다
namespace FoliageMeshes
{
	// 모르는 이름이면 false
	bool Build(std::string_view Name, FMeshData& OutMesh);
}

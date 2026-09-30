#pragma once

#include "Renderer/GltfLoader.h"

#include <filesystem>

// ufbx 기반 FBX 로더 → FModelData (glTF 로더와 같은 중간 데이터).
// 좌표: ufbx가 먼저 glTF와 같은 축(오른손, +Y 위, 미터)으로 바꾸고(지오메트리까지), 그 뒤 FGltfLoader::Convert*로
// 엔진 좌표계(왼손 Z-up, cm)로 옮긴다 — glTF 경로에서 검증된 변환을 그대로 공유하기 위해서다.
//   메시: 노드별 메시를 머티리얼 부분마다 FModelMesh 하나로 (삼각형화, 같은 정점은 합침)
//   스킨: 첫 스킨 디포머의 클러스터 = 조인트, geometry_to_bone = 역바인드 행렬, 정점당 가중치 상위 4개
//   애니메이션: 애니메이션 스택마다 ufbx_bake_anim으로 구운 키 → 클립
struct FFbxLoader
{
	static bool Load(const std::filesystem::path& Path, FModelData& OutModel);
};

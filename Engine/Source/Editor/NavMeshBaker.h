#pragma once

#include "AI/Navigation/NavMesh.h"
#include "Core/Math/Matrix4x4.h"

#include <filesystem>
#include <string>

class FResourceManager;
class FScene;
struct FTerrainData;
struct FTerrainFrame;

// 에디터 내비메시 굽기: 씬의 정적 지오메트리를 모아 FNavMesh를 굽고 .enav로 저장한다.
//   포함: 보이는 FStaticMeshComponent (모델 하위 메시 포함, 스킨 메시 제외), 충돌을 켠 지형(FTerrainComponent::bCollision — 높이장 격자,
//         셀 대각선 (0,0)-(1,1) = 렌더·충돌과 같은 면)
//   제외: 동적 강체, AI 에이전트(FBehaviorTreeComponent/FNavAgentComponent), 스크립트 컴포넌트(움직일 수 있음)와 그 하위 —
//         움직이는 물체는 바닥/장애물이 아니다 (스크립트가 붙은 고정 장애물은 스크립트를 자식 엔티티로 옮기거나 형제로 둔다)
struct FNavMeshBaker
{
	struct FResult
	{
		bool        bSucceeded = false;
		std::string Error;
		uint32      MeshCount     = 0;
		uint32      TriangleCount = 0; // 입력 삼각형
		int32       PolygonCount  = 0; // 구운 내비메시 폴리곤
	};

	// 엔진 월드 좌표 삼각형 (엔진 와인딩: Cross(P1 - P0, P2 - P0)가 앞면, 거울상 변환이면 뒤집는다)
	static FNavMeshBuildInput CollectInput(FScene& Scene, FResourceManager& Resources, uint32* OutMeshCount = nullptr);

	// 로컬 메시 하나를 World로 변환해 추가 (행렬식 < 0인 거울상 변환은 인덱스 순서를 뒤집어 앞면 방향 유지). 순수 로직
	static void AppendMesh(const std::vector<FVector3>& Positions, const std::vector<uint32>& Indices, const FMatrix4x4& World, FNavMeshBuildInput& Input);

	// 지형 높이장 전체를 월드 삼각형으로 (셀마다 둘, 대각선 (0,0)-(1,1), 위를 향하는 와인딩). 순수 로직
	static void AppendTerrain(const FTerrainData& Data, const FTerrainFrame& Frame, FNavMeshBuildInput& Input);

	// 씬의 FNavMeshComponent 설정으로 굽는다 (없으면 기본 설정). OutNavMesh는 성공했을 때만 바뀐다
	static FResult Bake(FScene& Scene, FResourceManager& Resources, FNavMesh& OutNavMesh);
};

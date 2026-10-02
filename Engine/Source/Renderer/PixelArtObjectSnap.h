#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <unordered_map>
#include <vector>

class FScene;

// 픽셀 아트 물체 도트 스냅 (렌더 동안만). 한 번이라도 움직인 최상위 루트의 하위 트리를 카메라와 같은 도트 격자에 맞추고
// Restore가 원래 월드 행렬 이동값으로 되돌린다 (게임·물리 위치 불변). 규칙은 cpp 머리 주석
class FPixelArtObjectSnap
{
public:
	// Right/Up/TexelWorldSize = 직교 카메라 스냅과 같은 값 (FSceneRenderer::BuildPixelArtCamera)
	void Apply(FScene& Scene, const FVector3& Right, const FVector3& Up, float TexelWorldSize);
	void Restore(FScene& Scene);

private:
	struct FTracked
	{
		FVector3 LastPosition;
		bool     bMoved = false;
	};
	struct FSaved
	{
		FEntity  Entity;
		FVector3 Origin;
	};

	void OffsetSubtree(FScene& Scene, FEntity Entity, const FVector3& Delta);

	const FScene*                         TrackedScene = nullptr; // 비교용(비소유) — 씬이 바뀌면 기록을 비운다
	std::unordered_map<FEntity, FTracked> Tracked;                // 최상위 루트별 지난 위치 + 움직인 적 있음
	std::vector<FSaved>                   Saved;                  // Apply가 바꾼 엔티티의 원래 이동값
};

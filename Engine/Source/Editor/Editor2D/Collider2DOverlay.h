#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Scene/Sprite/TilemapCollision.h"

#include <unordered_map>

struct FEditorContext;

// 뷰포트 충돌 모양 외곽선 (Phase 56-5a, ImGui 오버레이 — 깊이 무시, 뷰포트 위에 선):
//   2D 콜라이더(상자·원·캡슐·다각형·선분/체인 — Collider2DShapes, 물리와 같은 스케일·각), 트리거는 흐리게, 원웨이는 엔티티 로컬 위 방향 화살표,
//   타일맵 충돌(TilemapCollision::BuildShapes — Full 외곽선 체인·다각형 타일·원웨이 윗변 선분 + 위 화살표, 바뀔 때만 다시 만듦: Revision·타일셋·셀 크기),
//   2D 관절(앵커 +, 대상까지 선, 회전 = 원, 미닫이/바퀴 = 축선), 2D 캐릭터 이동기 캡슐, 3D 콜라이더(상자·구·캡슐 — 월드 행렬 근사).
//   대상 = 선택한 엔티티, bAll이면 씬 전체 (툴바 토글 / --show-colliders-2d)
class FCollider2DOverlay
{
public:
	void Draw(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize, bool bAll);

private:
	struct FTilemapCache
	{
		uint32                  Revision = 0;
		const void*             Tileset  = nullptr;
		FVector2                CellSize;
		FTilemapCollisionShapes Shapes;
		uint32                  LastFrame = 0;
	};
	std::unordered_map<uint64, FTilemapCache> TilemapCaches; // 엔티티 Id → 충돌 모양
	uint32                                    Frame = 0;
};

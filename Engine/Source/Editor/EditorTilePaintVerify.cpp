// FEditorApplication 자동 검증 중 2D 타일 칠하기 부분 (Phase 56-5a — EditorApplication.cpp 밖에 두어 다른 트랙과 겹치지 않게)
#include "Editor/EditorApplication.h"

#include "Editor/SceneEditOps.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

// --verify-tile-paint: 선택(없으면 첫) 타일맵에 스트로크 3개(브러시 선 / 회전한 2x1 스탬프 사각형 채우기 / 지우개)를 팔레트와 같은 경로로 칠하고
// 스트로크마다 Undo 한 단계인지, 실행 취소 3번 = 처음 TileData, 다시 실행 3번 = 칠한 TileData인지 확인한다. 씬은 저장하지 않는다 (끝 상태 = 칠한 결과)
void FEditorApplication::VerifyTilePaint()
{
	Scene.UpdateTransforms();
	FEntity Target = FTilePalettePanel::FindTarget(Context);
	if (!Target.IsValid())
	{
		Scene.GetRegistry().View<FTilemapComponent>().Each([&](FEntity Entity, FTilemapComponent&) {
			if (!Target.IsValid())
			{
				Target = Entity;
			}
		});
	}
	if (!Target.IsValid())
	{
		E_LOG(LogEditor, Error, "타일 칠하기 검증 실패: 타일맵 엔티티가 없습니다");
		return;
	}
	const FEntityPath TargetPath = FEntityPath::Build(Scene, Target);
	auto GetTilemap = [&]() -> FTilemapComponent* {
		const FEntity Entity = TargetPath.Resolve(Scene);
		return Entity.IsValid() ? Scene.GetRegistry().TryGet<FTilemapComponent>(Entity) : nullptr;
	};

	const std::string Before      = GetTilemap()->TileData;
	const size_t      UndoBefore  = UndoHistory.GetUndoCount();
	const int32       Columns     = [&]() {
        const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(*GetTilemap());
        return Tileset != nullptr ? Tileset->GetColumns() : 0;
	}();
	if (Columns < 2)
	{
		E_LOG(LogEditor, Error, "타일 칠하기 검증 실패: 타일셋 열이 2개 미만입니다");
		return;
	}

	// 1) 브러시: 타일 1을 (2, 9) → (6, 9) 선으로 (끄는 동안 셀만 바뀌고 Undo 기록 없음)
	const FTilePalettePanel::ETool SavedTool = TilePalettePanel.GetTool();
	TilePalettePanel.SetTool(FTilePalettePanel::ETool::Brush);
	TilePalettePanel.SetTransformFlags(0);
	TilePalettePanel.SetStamp(Editor2DMath::FTileStamp::Single(TileCell::Make(1)));
	TilePalettePanel.BeginStroke(Context, TargetPath.Resolve(Scene));
	TilePalettePanel.StrokeTo(Context, FTileCoord{ 2, 9 }, false);
	TilePalettePanel.StrokeTo(Context, FTileCoord{ 6, 9 }, false);
	const bool bNoCommitDuringStroke = GetTilemap()->TileData == Before && !Context.PendingEdit.IsPending();
	TilePalettePanel.EndStroke(Context);
	CommitPendingEdit();

	// 2) 사각형 채우기: 타일셋 첫 행 2칸 스탬프를 90도 돌려(1x2) (9, 9) ~ (11, 12)에 반복
	TilePalettePanel.SetStamp(Editor2DMath::MakeStampFromTileset(Columns, 0, 0, 1, 0));
	TilePalettePanel.SetTransformFlags(TileCell::Rotate90Bit);
	TilePalettePanel.BeginStroke(Context, TargetPath.Resolve(Scene));
	TilePalettePanel.ApplyRect(Context, FTileRect{ 9, 9, 11, 12 }, false);
	TilePalettePanel.EndStroke(Context);
	CommitPendingEdit();

	// 3) 지우개: (4, 9) 한 칸
	TilePalettePanel.SetTransformFlags(0);
	TilePalettePanel.SetStamp(Editor2DMath::FTileStamp::Single(TileCell::Make(0)));
	TilePalettePanel.BeginStroke(Context, TargetPath.Resolve(Scene));
	TilePalettePanel.StrokeTo(Context, FTileCoord{ 4, 9 }, true);
	TilePalettePanel.EndStroke(Context);
	CommitPendingEdit();
	TilePalettePanel.SetTool(SavedTool);

	const std::string   Painted = GetTilemap()->TileData;
	const FTilemapData& Data    = Sprite2DRuntime::GetTilemapData(*GetTilemap());
	// 회전한 스탬프 (가로 [타일 0, 타일 1] → 반시계 90도 = 세로 [아래 타일 0, 위 타일 1], 칸마다 Rotate90)
	const bool bCellsOk = Data.Get(2, 9) == TileCell::Make(1) && Data.Get(6, 9) == TileCell::Make(1) && Data.Get(4, 9) == 0u &&
	                      Data.Get(9, 9) == TileCell::Make(0, TileCell::Rotate90Bit) && Data.Get(9, 10) == TileCell::Make(1, TileCell::Rotate90Bit) &&
	                      Data.Get(11, 12) == TileCell::Make(1, TileCell::Rotate90Bit);
	const size_t UndoSteps = UndoHistory.GetUndoCount() - UndoBefore;

	for (int32 Step = 0; Step < 3; ++Step)
	{
		UndoEdit();
	}
	const bool bUndoOk = GetTilemap() != nullptr && GetTilemap()->TileData == Before;
	for (int32 Step = 0; Step < 3; ++Step)
	{
		RedoEdit();
	}
	const bool bRedoOk = GetTilemap() != nullptr && GetTilemap()->TileData == Painted;
	// 복원으로 엔티티 핸들이 바뀌었으므로 다시 선택 (화면 확인용)
	if (const FEntity Entity = TargetPath.Resolve(Scene); Entity.IsValid())
	{
		Context.Select(Entity);
	}

	const std::string Summary = std::format("Undo 단계 {} (기대 3), 끄는 중 커밋 없음 {}, 칸 {}, 실행 취소 {}, 다시 실행 {}", UndoSteps,
	                                        bNoCommitDuringStroke ? "예" : "아니오", bCellsOk ? "일치" : "불일치", bUndoOk ? "일치" : "불일치",
	                                        bRedoOk ? "일치" : "불일치");
	if (UndoSteps == 3 && bNoCommitDuringStroke && bCellsOk && bUndoOk && bRedoOk && Painted != Before)
	{
		E_LOG(LogEditor, Display, "타일 칠하기 검증 통과: {}", Summary);
	}
	else
	{
		E_LOG(LogEditor, Error, "타일 칠하기 검증 실패: {}", Summary);
	}
}

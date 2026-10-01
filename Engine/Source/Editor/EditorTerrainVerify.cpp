// FEditorApplication 자동 검증 중 지형 부분 (EditorApplication.cpp 밖에 두어 다른 트랙과 겹치지 않게)
#include "Editor/EditorApplication.h"

#include "Scene/Terrain.h"

#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

void FEditorApplication::VerifyTerrainBrush()
{
	// 지형이 없으면 하나 만든다 (Content/Terrain/Terrain_N.eterrain — 검증 뒤 지운다)
	Scene.UpdateTransforms();
	FEntity Terrain;
	Scene.GetRegistry().View<FTerrainComponent>().Each([&](FEntity Entity, FTerrainComponent&) {
		if (!Terrain.IsValid())
		{
			Terrain = Entity;
		}
	});
	bool bCreated = false;
	if (!Terrain.IsValid())
	{
		Terrain  = TerrainToolPanel.CreateTerrain(Context, 129, 10000.0f);
		bCreated = Terrain.IsValid();
		CommitPendingEdit();
	}
	if (!Terrain.IsValid())
	{
		E_LOG(LogEditor, Error, "지형 브러시 검증 실패: 지형을 만들 수 없음");
		return;
	}
	const std::string                   Asset = Scene.GetRegistry().Get<FTerrainComponent>(Terrain).Asset;
	const std::shared_ptr<FTerrainData> Data  = FTerrainLibrary::Get().Load(Asset);
	if (!Data)
	{
		E_LOG(LogEditor, Error, "지형 브러시 검증 실패: 데이터 없음 ({})", Asset);
		return;
	}
	const FVector3 Center = Scene.GetTransform(Terrain).GetWorldPosition();
	const float    Size   = Scene.GetRegistry().Get<FTerrainComponent>(Terrain).Size.X;

	const std::vector<uint16> Heights0 = Data->Heights;
	const std::vector<uint32> Weights0 = Data->Weights;

	// 1) 스컬프트: 가운데 올리기 0.5초
	FTerrainBrush& Brush = TerrainToolPanel.GetBrush();
	Brush.Op             = ETerrainBrushOp::Raise;
	Brush.Radius         = Size * 0.15f;
	Brush.Strength       = 1.0f;
	Brush.Falloff        = 0.5f;
	TerrainToolPanel.BeginStroke(Context, Terrain);
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		TerrainToolPanel.ApplyStroke(Context, FVector2(Center.X, Center.Y), 1.0f / 60.0f);
	}
	TerrainToolPanel.EndStroke(Context, "지형 스컬프트");
	CommitPendingEdit();
	const std::vector<uint16> Heights1 = Data->Heights;

	// 2) 칠하기: 레이어 1을 옆에 0.5초
	Brush.Op    = ETerrainBrushOp::Paint;
	Brush.Layer = 1;
	TerrainToolPanel.BeginStroke(Context, Terrain);
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		TerrainToolPanel.ApplyStroke(Context, FVector2(Center.X + Size * 0.2f, Center.Y), 1.0f / 60.0f);
	}
	TerrainToolPanel.EndStroke(Context, "지형 칠하기");
	CommitPendingEdit();
	const std::vector<uint32> Weights1 = Data->Weights;

	const bool bSculpted = Heights1 != Heights0;
	const bool bPainted  = Weights1 != Weights0;

	// 3) 되돌리기/다시 하기 (씬 스냅샷 복원 → 도구 Update가 편집 버전으로 데이터를 맞춘다)
	UndoEdit();
	TerrainToolPanel.Update(Context);
	const bool bUndoPaint = Data->Weights == Weights0 && Data->Heights == Heights1;
	UndoEdit();
	TerrainToolPanel.Update(Context);
	const bool bUndoSculpt = Data->Heights == Heights0 && Data->Weights == Weights0;
	RedoEdit();
	TerrainToolPanel.Update(Context);
	const bool bRedoSculpt = Data->Heights == Heights1 && Data->Weights == Weights0;
	RedoEdit();
	TerrainToolPanel.Update(Context);
	const bool bRedoPaint = Data->Heights == Heights1 && Data->Weights == Weights1;

	const std::string Summary = std::format("스컬프트 {}, 칠하기 {}, 취소(칠하기 {} / 스컬프트 {}), 다시(스컬프트 {} / 칠하기 {}), 기록 {}개 {:.1f}KB",
	                                        bSculpted, bPainted, bUndoPaint, bUndoSculpt, bRedoSculpt, bRedoPaint,
	                                        TerrainToolPanel.GetHistory().GetRecordCount(),
	                                        static_cast<double>(TerrainToolPanel.GetHistory().GetMemorySize()) / 1024.0);
	if (bSculpted && bPainted && bUndoPaint && bUndoSculpt && bRedoSculpt && bRedoPaint)
	{
		E_LOG(LogEditor, Display, "지형 브러시 검증 통과: {}", Summary);
	}
	else
	{
		E_LOG(LogEditor, Error, "지형 브러시 검증 실패: {}", Summary);
	}

	// 검증용으로 만든 지형 파일은 남기지 않는다 (메모리 데이터는 화면 확인용으로 유지)
	if (bCreated)
	{
		std::error_code             ErrorCode;
		const std::filesystem::path File = FTerrainLibrary::Get().ResolveAssetPath(Asset);
		std::filesystem::remove(File, ErrorCode);
		if (std::filesystem::is_empty(File.parent_path(), ErrorCode))
		{
			std::filesystem::remove(File.parent_path(), ErrorCode); // 검증이 만든 빈 Terrain 폴더
		}
	}
	TerrainToolPanel.SetMode(FTerrainToolPanel::EMode::Sculpt); // 스크린샷에 브러시 원이 보이게
	TerrainToolPanel.bAutomationCursor = true;
	TerrainToolPanel.bRequestFocus     = true;
	TerrainToolPanel.AutomationCursor  = FVector2(Center.X + Size * 0.2f, Center.Y);
	Brush.Radius                       = Size * 0.08f;
	// 편집 카메라: 칠한 곳을 비스듬히 내려다본다
	const FVector3 Focus(Center.X + Size * 0.2f, Center.Y, Center.Z);
	Camera.SetPosition(Focus + FVector3(-Size * 0.32f, -Size * 0.26f, Size * 0.2f));
	Camera.LookAt(Focus);
	CameraController.SyncFromCamera(Camera);
}

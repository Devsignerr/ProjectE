#pragma once

#include "Editor/AssetEditors/Sprite2DCanvas.h"
#include "Editor/AssetEditors/Sprite2DEditorBase.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <string>
#include <vector>

// 타일셋 편집기 (.etileset — Phase 56-5b).
//   왼쪽: 텍스처 캔버스 + 타일 격자(TileWidth/Height·Margin·Spacing 바꾸면 즉시) — 클릭/Ctrl+클릭/끌어 상자 선택(여러 타일),
//         오버레이 토글(충돌·원웨이·태그·애니메이션·번호)
//   오른쪽: 텍스처/격자 설정, 선택 타일 속성 — Collision(None/Full/Polygon, 여러 타일 한꺼번에), bOneWay, Tags(목록 — 여러 타일이면 공통 추가/제거),
//          Animation(타일 하나: 프레임 TileId + Duration, 미리보기), Polygon 점 편집(타일 확대 캔버스: 빈 곳 클릭 = 가까운 변에 점 끼우기,
//          점 끌기 = 이동(픽셀 스냅), 오른쪽 클릭 = 삭제 — 오목/8점 초과는 경고: 물리가 볼록 껍질로 바꾼다)
//   자동 검증 --verify-tileset-roundtrip, --tileset-select <Id,...>(선택)
class FTilesetEditor : public FSprite2DEditorBase
{
public:
	using FSprite2DEditorBase::FSprite2DEditorBase;

	const char* GetTypeName() const override { return "타일셋"; }
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        OnClose(FAssetEditorEnvironment& Env) override;

	const wchar_t* GetVerifyRoundTripFlag() const override { return L"--verify-tileset-roundtrip"; }
	bool           ApplyVerifyEdits(FAssetEditorEnvironment& Env) override;

private:
	void  Edited(std::string_view Label);
	void  SyncTexture(FAssetEditorEnvironment& Env);
	void  DrawCanvas(FAssetEditorEnvironment& Env);
	void  DrawTileOverlay(const FTileDefinition& Tile);
	void  DrawGridSettings(FAssetEditorEnvironment& Env);
	void  DrawTileProperties(FAssetEditorEnvironment& Env);
	void  DrawTags();
	void  DrawAnimation(FAssetEditorEnvironment& Env, int32 Id);
	void  DrawPolygonEditor(FAssetEditorEnvironment& Env, int32 Id);
	int32 FindTileAt(const FVector2& Point) const;
	bool  IsTileSelected(int32 Id) const;
	void  ClampSelection();
	// 선택한 모든 타일에 Fn(정의) 적용 후 SetTile
	template <typename TFunction>
	void  ForEachSelectedTile(TFunction&& Function);
	ImVec2 TileUv(int32 Id, bool bMax) const;

	FTilesetAsset   Asset;
	FTexturePreview Texture;
	FSprite2DCanvas Canvas;
	FSprite2DCanvas TileCanvas; // 다각형 편집 (선택 타일 확대)

	std::vector<int32> TileSelection; // 타일 Id (마지막 = 주 선택)
	bool               bBoxSelecting = false;
	FVector2           BoxStart;

	bool bShowCollision = true;
	bool bShowOneWay    = true;
	bool bShowTags      = true;
	bool bShowAnimation = true;
	bool bShowIds       = false;
	bool bSnapPoints    = true;

	int32 DraggingPoint = -1;
	float AnimTime      = 0.0f;
	bool  bCheckedArgs  = false;
	std::string NewTag;
};

#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Editor/Editor2D/Editor2DMath.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>

class FInput;
struct FEditorContext;
struct FTilemapComponent;

// 타일 팔레트 (창 "타일 팔레트", Phase 56-5a): 선택한 타일맵 엔티티의 타일셋 이미지에서 타일 하나/사각형(스탬프)을 고르고,
// 뷰포트에서 칠한다. 도구가 켜져 있으면(선택 도구가 아니면) 뷰포트 좌클릭이 도구가 된다 (기즈모/클릭 선택 대신 — 지형 도구와 같은 ToolOverlay).
//   도구: 선택(Q, 칠하기 끔) / 브러시(B) / 지우개(D) / 사각형 채우기(U) / 흐름 채우기(G) / 선(L) / 스포이드(I, 브러시 중 Alt = 잠깐 스포이드).
//   Shift = 지우기(브러시·사각형·선·흐름 채우기). 변환 토글: X = FlipX, Y = FlipY, Z = Rotate90 (스탬프 전체가 함께 돈다 — Editor2DMath::TransformStamp).
//   대상 = 주 선택(없으면 첫 선택)의 FTilemapComponent. 셀 = 커서 광선 ↔ 타일맵 로컬 평면 → TilemapMath::LocalToCell.
//   Undo: 스트로크(누름 ~ 뗌) 하나 = 한 단계 — 끄는 동안은 Runtime.Data만 고치고 MarkTilemapEdited(렌더러·충돌 갱신), 끝에
//   CommitTilemapData + MarkEdited("타일 칠하기") (지형 도구 규칙: 끄는 도중 MarkEdited 금지). 플레이 중에는 칠하지 않는다.
//   미리보기: 브러시·사각형·선 = 반투명 실제 타일(FSceneRenderer::SetSpriteDrawList — 컴포넌트 밖 그리기 목록), 그 밖은 칸 외곽선.
class FTilePalettePanel
{
public:
	enum class ETool : int32
	{
		Select,
		Brush,
		Eraser,
		Rect,
		Flood,
		Line,
		Picker,
	};

	void Draw(FEditorContext& Context);
	// 뷰포트 이미지 위 (ImGui 프레임 안). 반환: 이번 프레임 마우스를 가져갔다 (기즈모/클릭 선택 생략)
	bool HandleViewport(FEditorContext& Context, const FInput& Input, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered);
	// 뷰포트를 그린 뒤 매 프레임: 이번 프레임 미리보기를 쓰지 않았으면 그리기 목록을 비운다
	void FinishFrame(FEditorContext& Context);
	bool IsActive() const { return bOpen && Tool != ETool::Select; }

	// ---- 자동 검증/스크립트용 (뷰포트와 같은 경로)
	void  SetTool(ETool NewTool) { Tool = NewTool; }
	ETool GetTool() const { return Tool; }
	void  SetStamp(const Editor2DMath::FTileStamp& Stamp) { BaseStamp = Stamp; }
	void  SetTransformFlags(uint32 Flags) { TransformFlags = Flags; }
	Editor2DMath::FTileStamp GetEffectiveStamp() const;
	bool  BeginStroke(FEditorContext& Context, FEntity Target);
	void  StrokeTo(FEditorContext& Context, const FTileCoord& Cell, bool bErase); // 브러시/지우개: 직전 셀에서 선으로 이어 칠함
	int32 ApplyRect(FEditorContext& Context, const FTileRect& Rect, bool bErase);
	int32 ApplyLine(FEditorContext& Context, const FTileCoord& From, const FTileCoord& To, bool bErase);
	int32 ApplyFlood(FEditorContext& Context, const FTileCoord& Start, bool bErase); // -1 = 영역이 너무 큼
	void  EndStroke(FEditorContext& Context);
	// 셀의 타일·플래그를 집어 스탬프로 (빈칸이면 false)
	bool  PickCell(FEditorContext& Context, FEntity Target, const FTileCoord& Cell);
	static FEntity FindTarget(const FEditorContext& Context);

	bool bOpen         = true;
	bool bRequestFocus = false; // 다음 몇 번의 Draw에서 창을 앞으로 (기본 배치 적용·다른 탭 그리기 순서와 겹쳐도 남게)
	// 자동 검증 스크린샷용: 마우스가 뷰포트 밖이면 이 셀을 커서 칸으로 삼아 미리보기를 그린다 (--tile-preview-cell X,Y)
	bool       bAutomationCell = false;
	FTileCoord AutomationCell;

private:
	FTilemapComponent* GetStrokeTilemap(FEditorContext& Context) const;
	void HandleShortcuts();
	void DrawToolbar();
	void DrawPalette(FEditorContext& Context, FEntity Target);
	void DrawPreview(FEditorContext& Context, FEntity Target, const FVector2& ImageMin, const FVector2& ImageSize);
	// 팔레트 이미지 (편집기 소유 UNORM 텍스처 — 2D 에셋 편집기와 같은 방식: ImGui는 UNORM 백버퍼라 sRGB 텍스처는 어둡게 보인다).
	// 경로가 바뀔 때만 다시 읽는다 (이전 것은 DestroyTexture 지연 해제)
	void UpdatePaletteTexture(FEditorContext& Context, const std::string& ContentPath);

	ETool                    Tool           = ETool::Select;
	ETool                    LastPaintTool  = ETool::Brush;
	Editor2DMath::FTileStamp BaseStamp      = Editor2DMath::FTileStamp::Single(TileCell::Make(0));
	uint32                   TransformFlags = 0;
	float                    Zoom           = 4.0f;

	// 팔레트 선택 (열·행, 이미지 왼쪽 위 원점)
	int32 SelectCol0 = 0;
	int32 SelectRow0 = 0;
	int32 SelectCol1 = 0;
	int32 SelectRow1 = 0;
	bool  bSelecting = false;

	// 스트로크
	bool       bStroking      = false;
	ETool      StrokeTool     = ETool::Brush;
	bool       bStrokeErase   = false;
	FEntity    StrokeEntity;
	FTileCoord StrokeStart;
	FTileCoord StrokeLast;
	int32      StrokeChanged  = 0;
	bool       bStrokeHasLast = false; // StrokeLast가 유효 (브러시 첫 칸 뒤)

	// 커서 셀 (직전 HandleViewport)
	bool       bHasCell = false;
	FTileCoord HoverCell;

	std::filesystem::path PaletteTexturePath;
	FTextureHandle        PaletteTexture;
	int32                 PaletteTextureWidth   = 0;
	int32                 PaletteTextureHeight  = 0;
	bool                  bPaletteTextureFailed = false;

	int32 FocusFrames      = 0;
	bool bPreviewSet       = false; // SetSpriteDrawList로 미리보기를 넣어 둠
	bool bPreviewThisFrame = false;
};

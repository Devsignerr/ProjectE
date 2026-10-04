#pragma once

#include "Editor/AssetEditors/Sprite2DCanvas.h"
#include "Editor/AssetEditors/Sprite2DEditing.h"
#include "Editor/AssetEditors/Sprite2DEditorBase.h"
#include "Scene/Sprite/SpriteAsset.h"

#include <string>
#include <vector>

// 스프라이트 아틀라스 편집기 (.esprite — Phase 56-5b).
//   왼쪽: 텍스처 캔버스(점 필터 확대, 체커 배경, 픽셀 격자) — 슬라이스 사각형 표시·선택(Ctrl 여러 개)·이동·크기 조절(픽셀 스냅),
//         빈 곳 끌기 = 새 슬라이스, 선택 슬라이스의 피벗 점 끌기, 9-슬라이스 테두리 선 끌기(테두리 표시 켬), Delete = 삭제
//   오른쪽: 텍스처(드롭/목록 — 크기는 이미지에서 자동), UnitsPerPixel, Filter, 슬라이스 목록(이름 바꾸기 — 중복 검사, 정렬, 삭제),
//          선택 슬라이스 사각형/피벗(프리셋)/테두리, "격자로 자르기" 대화(셀 크기 또는 개수, 여백, 간격, 접두사, 빈 칸 건너뛰기, 교체/추가)
//   편집 연산은 Sprite2DEditing. 자동 검증 --verify-sprite-roundtrip (Sprite2DEditorBase), --sprite-select <슬라이스 이름>(선택),
//   --sprite-grid-dialog(격자 대화 열기)
class FSpriteAtlasEditor : public FSprite2DEditorBase
{
public:
	using FSprite2DEditorBase::FSprite2DEditorBase;

	const char* GetTypeName() const override { return "스프라이트 아틀라스"; }

	// 콘텐츠 브라우저 "스프라이트 아틀라스 만들기 (격자로 자르기)": 다음에 그 경로를 열 때 격자 대화를 띄운다
	static void RequestGridDialog(const std::filesystem::path& AssetPath);

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        OnClose(FAssetEditorEnvironment& Env) override;

	const wchar_t* GetVerifyRoundTripFlag() const override { return L"--verify-sprite-roundtrip"; }
	bool           ApplyVerifyEdits(FAssetEditorEnvironment& Env) override;

private:
	enum class EDrag : int32
	{
		None,
		Rect,   // 선택 슬라이스 이동/크기
		Create, // 새 슬라이스
		Pivot,
		Border,
	};

	void Edited(std::string_view Label);
	void SyncTexture(FAssetEditorEnvironment& Env);
	void DrawCanvas(FAssetEditorEnvironment& Env);
	void HandleCanvasInput();
	void DrawSliceList();
	void DrawSliceDetails();
	void DrawGridDialog();
	void DeleteSelected();
	void SelectOnly(int32 Index);
	bool IsSelected(int32 Index) const;
	int32 GetPrimary() const { return Selection.empty() ? -1 : Selection.back(); }
	void ClampSelection();
	int32 FindSliceAt(const FVector2& Point) const;

	FSpriteAsset    Asset;
	FTexturePreview Texture;
	FSprite2DCanvas Canvas;

	std::vector<int32> Selection; // 마지막 = 주 선택
	EDrag              Drag       = EDrag::None;
	Sprite2DEditing::ERectHandle DragHandle = Sprite2DEditing::ERectHandle::None;
	int32              DragBorderSide = -1;
	FVector2           DragStart;
	Sprite2DEditing::FPixelRect DragOriginalRect;

	bool bShowPivots  = true;
	bool bShowBorders = true;
	bool bShowNames   = true;
	bool bShowGrid    = true;

	// 이름 바꾸기 (목록 더블 클릭 / 상세 칸)
	int32       RenamingIndex = -1;
	std::string RenameBuffer;
	std::string RenameError;
	bool        bFocusRename = false;

	// 격자로 자르기 대화
	bool                              bOpenGridDialog = false;
	bool                              bGridDialogOpen = false;
	Sprite2DEditing::FGridSliceOptions GridOptions;
	bool                              bGridReplace = true;
	bool                              bCheckedArgs = false;
};

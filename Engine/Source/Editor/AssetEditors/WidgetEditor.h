#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "UI/UIAsset.h"
#include "UI/UIDrawList.h"
#include "UI/UIInput.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class FD3D12RenderTarget;
struct ImDrawList;

// .eui UI 디자이너 (UMG 위젯 블루프린트 편집기에 해당).
//   왼쪽: 계층(선택/끌어서 부모 바꾸기/순서) + 팔레트(끌어서 캔버스·계층에 놓기)
//   가운데: 캔버스 — 설계 해상도(또는 미리보기 해상도) 화면을 실제 UI 렌더러로 그리고, 선택 윤곽/핸들/앵커를 ImGui로 겹친다.
//           휠 = 확대, 가운데/오른쪽 드래그 = 이동, 왼쪽 = 선택·이동, 핸들 = 크기 (캔버스 자식만). "미리보기 입력"을 켜면 호버/클릭을 실제로 처리한다
//   오른쪽: UI 설정 + 선택한 위젯의 공통/슬롯(부모 종류별)/종류별 속성
// 선택은 루트로부터의 자식 번호 경로로 기억한다 (실행 취소로 트리를 다시 만들어도 유지).
class FWidgetEditor final : public FAssetEditor
{
public:
	explicit FWidgetEditor(std::filesystem::path InPath);
	~FWidgetEditor() override;

	const char* GetTypeName() const override { return "UI"; }
	float       GetPropertiesWidthWeight() const override { return 0.75f; }
	void        RenderPreview(FAssetEditorEnvironment& Env) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        FramePreview(FAssetEditorEnvironment& Env) override;
	void        OnClose(FAssetEditorEnvironment& Env) override;

private:
	enum class EDragMode : uint8
	{
		None,
		Move,
		Resize,
		Pan,
	};

	// ---- 패널
	void DrawOutliner(FAssetEditorEnvironment& Env);
	void DrawHierarchyNode(FUIWidget& Widget);
	void DrawPalette();
	void DrawCanvasToolbar();
	void DrawCanvas(FAssetEditorEnvironment& Env);
	void HandleCanvasInput(bool bHovered, bool bActive);
	void HandlePreviewInput(bool bHovered);
	void DrawCanvasOverlay(ImDrawList* DrawList);
	void HandleShortcuts();

	// ---- 애니메이션 타임라인 (캔버스 아래)
	void          DrawTimeline();
	void          DrawTimelineTracks(FUIAnimation& Animation);
	void          DrawKeyPanel(FUIAnimation& Animation);
	void          AddKeysForSelected(FUIAnimation& Animation, std::initializer_list<EUIAnimProperty> Properties);
	FUIAnimation* GetSelectedAnimation();
	std::string   MakeUniqueAnimationName(std::string_view Base, const FUIAnimation* Except) const;
	// 표시용 트리: 타임라인이 열려 있으면 재생 헤드 시점 애니메이션을 적용한 복제본, 아니면 편집 트리
	void       UpdateAnimationPreview(float DeltaSeconds);
	FUIWidget& GetDisplayRoot() { return AnimPreview ? *AnimPreview->Root : *Asset.Root; }

	// ---- 속성
	void DrawAssetSettings();
	void DrawCommonProperties(FUIWidget& Widget);
	void DrawSlotProperties(FUIWidget& Widget);
	void DrawTypeProperties(FUIWidget& Widget);
	bool DrawBrush(const char* Label, FUIBrush& Brush, bool bDefaultOpen);
	bool DrawTextureField(const char* Label, std::string& Path);
	void DrawAnchorPresets(FUIWidget& Widget);

	// ---- 트리 편집 (계층/팔레트 순회가 끝난 뒤 한 번에 적용)
	void       Defer(std::function<void()> Action) { PendingAction = std::move(Action); }
	FUIWidget* AddWidget(EUIWidgetType Type, FUIWidget* Parent, int32 Index, const FVector2* DropPointUi);
	void       DeleteWidget(FUIWidget* Widget);
	void       DuplicateWidget(FUIWidget* Widget);
	void       PasteInto(FUIWidget* Destination);
	void       MoveWidget(FUIWidget* Widget, FUIWidget* NewParent, int32 Index);
	std::string MakeUniqueName(std::string_view Base, const FUIWidget* Except) const;
	void        MakeNamesUnique(FUIWidget& Subtree);

	// ---- 선택
	FUIWidget*         GetSelected();
	void               Select(const FUIWidget* Widget);
	std::vector<int32> GetPath(const FUIWidget* Widget) const;
	FUIWidget*         ResolvePath(const std::vector<int32>& Path);
	static FUIWidget*  ResolvePathIn(FUIWidget& Root, const std::vector<int32>& Path);

	// ---- 레이아웃/좌표 (캔버스 타깃 픽셀 = 화면 좌표 - CanvasMin)
	void       UpdateLayout();
	float      GetUiScale() const;
	FVector2   ScreenToUi(const FVector2& Screen) const;
	FVector2   UiToScreen(const FVector2& Ui) const;
	FUIWidget* PickWidget(FUIWidget& Widget, const FVector2& UiPoint);
	void       ScanContentFiles(FAssetEditorEnvironment& Env);

	FUIAsset Asset;
	std::vector<int32> SelectedPath;
	bool               bHasSelection = false;
	std::function<void()> PendingAction;

	// 뷰
	float    Zoom          = 0.5f;
	FVector2 Pan;                       // 캔버스 타깃 픽셀 기준 화면(프레임) 좌상단
	FVector2 PreviewSize   = FVector2(1920.0f, 1080.0f); // 미리보기 해상도 (앵커 확인용)
	int32    PreviewPreset = 0;          // 0 = 설계 해상도
	bool     bFitRequested = true;
	bool     bViewTouched  = false; // 휠/드래그로 보기를 바꿨으면 창 크기 변화에 자동 맞춤하지 않는다
	FVector2 LastCanvasSize;
	bool     bShowBounds   = true;
	bool     bPreviewInput = false;
	bool     bAnchorMovesWidget = false; // 앵커 프리셋: 피벗/위치도 앵커로
	float    SnapStep      = 1.0f;
	FVector2 CanvasMin;                 // 화면 좌표
	FVector2 CanvasSize;
	bool     bCanvasHovered = false;

	// 캔버스 드래그
	EDragMode Drag       = EDragMode::None;
	int32     DragHandle = -1; // 0~7: 좌상, 상, 우상, 우, 우하, 하, 좌하, 좌
	FVector2  DragStartMouse;
	FUIRect   DragStartRect;
	FUISlot   DragStartSlot;
	FUIWidget* HoveredWidget = nullptr; // 이번 프레임 캔버스에서 포인터 아래 위젯 (프레임 안에서만 유효)

	// 렌더
	std::unique_ptr<FD3D12RenderTarget> Target;
	FUIDrawList                         DrawList;
	bool                                bRenderThisFrame = false;
	std::filesystem::path               ContentDirectory;

	// 미리보기 입력
	FUIInputRouter           PreviewRouter;
	std::vector<std::string> EventLog;

	// 콘텐츠 파일 목록 (Content 기준)
	std::vector<std::string> ImageFiles;
	std::vector<std::string> FontFiles;

	// 애니메이션 타임라인
	bool                      bShowTimeline      = false;
	int32                     SelectedAnimation  = -1;
	float                     Playhead           = 0.0f;
	bool                      bTimelinePlaying   = false;
	bool                      bTimelineLoop      = true;
	int32                     SelectedTrack      = -1;
	int32                     SelectedKey        = -1;
	bool                      bDraggingKey       = false;
	bool                      bDraggingPlayhead  = false;
	std::unique_ptr<FUIAsset> AnimPreview;
	char                      AnimationNameBuffer[64] = {};
	int32                     AnimationNameIndex = -1;

	// 자동 검증 인자 (--ui-select / --ui-zoom)
	std::string AutoSelectName;
	float       AutoZoom = 0.0f;
	std::string AutoAnimation; // --ui-animation <이름> [--ui-anim-time <초>]: 타임라인을 열고 그 시점 미리보기
	float       AutoAnimationTime = 0.0f;
	// 자동 검증 (Phase 32-2): --ui-text-demo select|compose — 선택한 텍스트 상자를 미리보기 입력으로 포커스해 선택 영역/IME 조합 표시를 보여 준다
	std::string    AutoTextDemo;
	std::u32string AutoComposition;

	// 이름 편집 버퍼 (선택이 바뀌면 다시 채움)
	char               NameBuffer[128] = {};
	std::vector<int32> NameBufferPath;
};

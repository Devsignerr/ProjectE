#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "Editor/SnapSettings.h"

#include <memory>

class FD3D12RenderTarget;
class FEditorGrid;
class FSelectionOutline;
class FInput;
struct FEditorContext;

// 씬 뷰포트: 오프스크린 렌더 타깃을 ImGui 이미지로 표시, 기즈모, 클릭 선택
class FViewportPanel
{
public:
	FViewportPanel();
	~FViewportPanel();

	// 렌더 타깃 해제 (GPU 유휴 상태에서 호출)
	void Shutdown();

	// 프레임 시작 시(ImGui NewFrame 전) 크기 변경 반영
	void PrepareFrame(FEditorContext& Context);

	// UI 기술 (ImGui 프레임 안에서). 기즈모 조작 결과는 즉시 씬에 반영된다.
	void Draw(FEditorContext& Context, const FInput& Input);

	// 씬을 렌더 타깃에 그린다 (RHI BeginFrame 이후)
	void RenderScene(FEditorContext& Context);

	// 선택 아웃라인 셰이더 핫 리로드
	bool ReloadShaders(bool bForceRecompile);

	// 선택 대상 전체가 보이도록 카메라를 물린다 (F). 선택이 없으면 아무것도 하지 않는다
	void FocusSelection(FEditorContext& Context);

	bool IsHovered() const { return bHovered; }
	bool IsUsingGizmo() const { return bUsingGizmo; }
	bool bOpen = true;

	bool          bShowGrid = true;
	FSnapSettings Snap;

private:
	using EGizmoOperation = ETransformTool;

	void DrawToolbar();
	void RenderGrid(FEditorContext& Context);
	void DrawGizmo(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize);
	void PickEntity(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize);

	std::unique_ptr<FD3D12RenderTarget> RenderTarget;
	std::unique_ptr<FSelectionOutline>  SelectionOutline;
	std::unique_ptr<FEditorGrid>        Grid;
	uint32                              DesiredWidth  = 0;
	uint32                              DesiredHeight = 0;

	EGizmoOperation GizmoOperation = EGizmoOperation::Translate;
	bool            bGizmoLocal    = false;
	bool            bHovered       = false;
	bool            bUsingGizmo    = false;
	bool            bGizmoOver     = false;
};

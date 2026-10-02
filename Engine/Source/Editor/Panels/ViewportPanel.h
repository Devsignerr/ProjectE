#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Editor/SnapSettings.h"
#include "UI/UIDrawList.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class FCamera;
class FD3D12RenderTarget;
class FEditorGrid;
class FNavMeshDebugRenderer;
class FSelectionOutline;
class FUIRenderer;
class FInput;
struct FRay;
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
	// 뷰포트 창이 키보드 포커스를 가졌는지 (직전 프레임 기준, 씬 편집 단축키 대상 판정)
	bool IsFocused() const { return bFocused; }
	// 뷰포트 렌더 타깃 종횡비 (타깃이 없으면 Fallback)
	float GetAspectRatio(float Fallback) const;
	bool IsUsingGizmo() const { return bUsingGizmo; }
	// 플레이 중 게임 UI: 뷰포트 이미지 좌상단(화면 좌표, 직전 프레임)과 UI 영역(렌더 타깃 픽셀, 이미지와 1:1)
	FVector2 GetImageMin() const { return ImageMin; }
	FUIRect  GetGameUIViewport() const;
	// 게임 UI가 포인터를 가져간 프레임에는 클릭 선택을 하지 않는다 (에디터가 매 프레임 설정)
	bool bGameUIWantsPointer  = false;
	bool bGameUIWantsKeyboard = false; // 게임 UI 텍스트 상자에 입력 중 (ESC로 플레이를 멈추지 않는다)
	bool bOpen = true;

	bool          bShowGrid    = true;
	bool          bShowNavMesh = false; // 내비메시(+ 플레이 중 이동 경로) 디버그 표시
	FSnapSettings Snap;

	// 표시할 내비메시 (FNavMesh::GetDebugTriangles, 엔진 좌표). 다음 렌더에서 GPU 버퍼로 올린다. 빈 목록 = 지움
	void SetNavMeshTriangles(std::vector<FVector3> Triangles);

	// 편집 도구 오버레이 (지형/폴리지 브러시): (Context, Input, 이미지 좌상단, 이미지 크기, 마우스 위) → true면 기즈모/클릭 선택 생략
	std::function<bool(FEditorContext&, const FInput&, const FVector2&, const FVector2&, bool)> ToolOverlay;
	// 화면 통계 오버레이 (콘솔 stat fps/gpu): (이미지 좌상단, 이미지 크기) — 툴바 뒤에 같은 창 그리기 목록에
	std::function<void(const FVector2&, const FVector2&)> StatOverlay;

private:
	using EGizmoOperation = ETransformTool;

	void DrawToolbar(FEditorContext& Context);
	// 편집 카메라 원근 ↔ 직교 전환 (플레이 중에는 게임 카메라가 쓰이므로 호출하지 않는다)
	void ToggleOrthographic(FCamera& Camera);
	void RenderGrid(FEditorContext& Context);
	void RenderNavMeshDebug(FEditorContext& Context);
	void RenderGameUI(FEditorContext& Context);
	void DrawGizmo(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize);
	void PickEntity(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize);
	// 커서 아래 가장 가까운 메시 엔티티 (모델 하위 노드 그대로). OutRay/OutDistance: 광선과 경계 상자 진입 거리
	FEntity RaycastMesh(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FRay& OutRay, float& OutDistance) const;
	// 콘텐츠 브라우저 드롭: 모델/파티클은 놓은 위치에 추가, 머티리얼은 커서 아래 메시에 지정
	void HandleAssetDrop(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths, const FVector2& LocalPixel, const FVector2& ImageSize);

	std::unique_ptr<FD3D12RenderTarget> RenderTarget;
	std::unique_ptr<FSelectionOutline>  SelectionOutline;
	std::unique_ptr<FEditorGrid>        Grid;
	std::unique_ptr<FUIRenderer>        UIRenderer; // 플레이 중 게임 UI (처음 필요할 때)
	std::unique_ptr<FNavMeshDebugRenderer> NavMeshDebug;           // 내비메시 표시를 켤 때 만든다
	std::vector<FVector3>                  PendingNavMeshTriangles;
	bool                                   bNavMeshTrianglesDirty = false;
	FUIDrawList                         GameUIDrawList;
	FVector2                            ImageMin;
	uint32                              DesiredWidth  = 0;
	uint32                              DesiredHeight = 0;

	EGizmoOperation GizmoOperation = EGizmoOperation::Translate;
	bool            bGizmoLocal    = false;
	bool            bHovered       = false;
	bool            bUsingGizmo    = false;
	bool            bGizmoOver     = false;
	bool            bFocused       = false;
	bool            bWasUsingGizmo = false; // 기즈모 조작 시작 프레임 판정 (Alt+드래그 복제)
};

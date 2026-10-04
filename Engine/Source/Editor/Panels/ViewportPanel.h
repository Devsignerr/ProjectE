#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Editor/Editor2D/Viewport2DCamera.h"
#include "Editor/SnapSettings.h"
#include "UI/UIDrawList.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class FCamera;
class FCollider2DOverlay;
class FD3D12RenderTarget;
class FEditorGrid;
class FNavMeshDebugRenderer;
class FDebugDrawRenderer;
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

	// 스크립트 디버거 정지 중: 마지막으로 그린 화면을 그대로 보여 주고 안내만 겹친다 (입력·기즈모·선택·드롭 없음, 렌더 타깃 크기 유지)
	void DrawFrozen(const char* Message);

	// 씬을 렌더 타깃에 그린다 (RHI BeginFrame 이후)
	void RenderScene(FEditorContext& Context);

	// 선택 아웃라인 셰이더 핫 리로드
	bool ReloadShaders(bool bForceRecompile);

	// 선택 대상 전체가 보이도록 카메라를 물린다 (F). 선택이 없으면 아무것도 하지 않는다
	void FocusSelection(FEditorContext& Context);
	// 2D 모드: 씬의 모든 2D 표시(스프라이트·타일맵·2D 콜라이더)가 보이도록 (자동 검증 --viewport-2d에서 선택이 없을 때)
	void FrameAll2D(FEditorContext& Context);

	bool IsHovered() const { return bHovered; }
	// 뷰포트 창이 키보드 포커스를 가졌는지 (직전 프레임 기준, 씬 편집 단축키 대상 판정)
	bool IsFocused() const { return bFocused; }
	// 뷰포트 렌더 타깃 종횡비 (타깃이 없으면 Fallback)
	float GetAspectRatio(float Fallback) const;
	bool IsUsingGizmo() const { return bUsingGizmo; }
	// 자동 검증(--verify-pick): Target 메시 경계 중심이 보이는 화면 위치를 클릭한 것처럼 선택하고 선택된 엔티티를 돌려준다.
	//   bFocus면 먼저 편집 카메라를 대상에 맞춘다(F). 대상이 화면 밖이면 false (선택하지 않음)
	bool VerifyPick(FEditorContext& Context, FEntity Target, bool bFocus, FEntity& OutPicked);
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
	float*        CameraSpeed = nullptr; // 편집 카메라 이동 속도 (cm/초, 비소유 — FEditorApplication의 FFlyCameraController::MoveSpeed). 툴바에서 고친다

	// ---- 2D 모드 (Phase 56-5a — 규칙은 CLAUDE.md "2D 에디터"): 편집 카메라를 +Y에서 -Y를 보는 직교로 고정, 휠 = 커서 기준 줌,
	//   가운데/오른쪽 드래그 = 팬, 격자 = X-Z 평면, 기즈모 = 평면 제약(이동 X/Z, 회전 Y, 스케일 X/Z), 2D 스냅(격자/픽셀)
	enum class ESnap2D : int32
	{
		Off,
		Grid,  // 격자 칸 (선택 타일맵 셀, 없으면 Grid2DCellSize) — 원점 = 격자 원점
		Pixel, // 선택 스프라이트/타일맵 도트 (UnitsPerPixel)
	};
	FCamera* EditCamera = nullptr; // 편집 카메라 (비소유 — 2D 조작은 Context.Camera가 이것일 때만)
	bool    bShowAllColliders2D = false; // 씬 전체 충돌 모양 외곽선 (끄면 선택한 것만)
	ESnap2D Snap2D              = ESnap2D::Off;
	float   Grid2DCellSize      = 50.0f; // cm (타일맵을 고르지 않았을 때 2D 격자 칸)
	bool    Is2DMode() const { return Camera2D.IsEnabled(); }
	void    Set2DMode(FEditorContext& Context, bool bEnable);
	FViewport2DCamera&       GetCamera2D() { return Camera2D; }
	const FViewport2DCamera& GetCamera2D() const { return Camera2D; }

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
	// 2D 격자 칸·원점·도트 간격 (선택 타일맵/스프라이트 기준, 매 프레임 Draw에서)
	void Update2DGrid(FEditorContext& Context);
	// 2D 모드 카메라 입력 (뷰포트 위)
	void Handle2DCamera(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize);
	void RenderNavMeshDebug(FEditorContext& Context);
	void RenderDebugDraw(FEditorContext& Context); // FDebugDraw 선 (Lua Debug.* / 게임 모듈 — 편집·플레이 모두)
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
	std::unique_ptr<FDebugDrawRenderer>    DebugDrawRenderer;      // 디버그 선이 처음 생길 때 만든다
	std::unique_ptr<FCollider2DOverlay>    ColliderOverlay;
	FViewport2DCamera                      Camera2D;
	FVector2                               Grid2DCell   = FVector2(50.0f, 50.0f);
	FVector2                               Grid2DOrigin = FVector2::ZeroVector;
	FVector2                               Grid2DPixel  = FVector2::ZeroVector; // 0 = 도트 기준 없음
	float                                  Grid2DDepth  = 0.0f;
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

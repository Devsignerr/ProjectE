#pragma once

#include "Core/Math/Math.h"
#include "Editor/AssetEditors/OrbitCamera.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"

#include <memory>

class FD3D12RenderTarget;
class FD3D12RHI;
class FEditorGrid;
class FResourceManager;
class FSceneRenderer;

// 에셋 편집 창의 미리보기: 전용 씬 + 궤도 카메라 + 고정 크기 오프스크린 타깃.
// 모든 미리보기는 같은 전용 씬 렌더러(FAssetEditorManager 소유)로 그리므로 타깃 크기를 고정해
// 렌더러의 HDR/블룸 버퍼가 창마다 다시 만들어지지 않게 한다. 창에는 종횡비를 유지해 맞춰 표시한다.
class FAssetPreview
{
public:
	static constexpr uint32 Width  = 960;
	static constexpr uint32 Height = 720;

	FAssetPreview();
	~FAssetPreview();

	bool Init(FD3D12RHI& Rhi);
	void Shutdown(FD3D12RHI& Rhi); // 지연 해제 (프레임 진행 중 호출 가능)

	FScene&        GetScene() { return Scene; }
	FCamera&       GetCamera() { return Camera; }
	FOrbitCamera&  GetOrbit() { return Orbit; }
	const FCamera& GetCamera() const { return Camera; }

	// 기본 조명 (방향광 1개) 엔티티 생성
	void AddDefaultLight();
	// 씬의 메시 전체 경계 (없으면 무효 상자)
	FBox ComputeMeshBounds(const FResourceManager& Resources);
	// 메시 전체가 보이도록 카메라를 맞춘다. 메시가 없으면 Fallback 경계 사용
	void FrameMeshes(const FResourceManager& Resources, const FBox& Fallback);

	// UI: 남은 영역(또는 Size)에 미리보기 이미지를 그리고 마우스 조작(좌/우 드래그 회전, 가운데 드래그 이동, 휠 확대)을 처리한다.
	void DrawViewport(const FVector2& Size);
	bool IsHovered() const { return bHovered; }
	// DrawViewport가 차지한 화면 영역 (오버레이 클립용)
	FVector2 GetViewportMin() const { return ViewportMin; }
	FVector2 GetViewportMax() const { return ViewportMax; }
	bool WasDrawnThisFrame() const { return bDrawn; }
	void BeginUiFrame() { bDrawn = false; }

	// 월드 점 → 화면 좌표 (DrawViewport가 그린 이미지 기준). 카메라 뒤면 false
	bool ProjectToScreen(const FVector3& WorldPosition, FVector2& OutScreen) const;

	// 씬을 타깃에 그린다 (Rhi BeginFrame 이후). Grid가 있으면 바닥 그리드 합성
	void Render(FD3D12RHI& Rhi, FSceneRenderer& Renderer, FEditorGrid* Grid);

	bool bShowGrid = true;

private:
	FScene                              Scene;
	FCamera                             Camera;
	FOrbitCamera                        Orbit;
	std::unique_ptr<FD3D12RenderTarget> Target;

	FVector2 ViewportMin;
	FVector2 ViewportMax;
	FVector2 ImageMin;
	FVector2 ImageSize;
	bool     bHovered = false;
	bool     bDrawn   = false;

	void SyncCamera();
};

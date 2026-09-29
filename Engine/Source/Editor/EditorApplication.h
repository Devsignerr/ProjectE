#pragma once

#include "Core/Application.h"
#include "Editor/EditorContext.h"
#include "Editor/ImGuiLayer.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ViewportPanel.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <filesystem>
#include <memory>

class FD3D12RHI;

// 에디터 애플리케이션: RHI/리소스/씬/렌더러 + ImGui 패널 + 씬 파일 관리
class FEditorApplication final : public FApplication
{
public:
	FEditorApplication();
	~FEditorApplication() override; // unique_ptr<FD3D12RHI> 완전 타입이 필요하므로 cpp에 정의

protected:
	bool OnInit() override;
	void OnUpdate(float DeltaSeconds) override;
	void OnRender() override;
	void OnResize(uint32 Width, uint32 Height) override;
	void OnShutdown() override;

private:
	// ---- 씬 파일
	void NewScene();
	bool OpenScene(const std::filesystem::path& Path);
	bool SaveScene();
	bool SaveSceneAs();
	void OpenStartupScene(); // 프로젝트 기본 씬 로드 (없으면 기본 씬 구성 후 생성)
	void UpdateWindowTitle();

	void BuildDefaultScene();
	void DrawMainMenuBar();
	void DrawStatsWindow();
	void HandleShortcuts();

	std::unique_ptr<FD3D12RHI> Rhi;
	FResourceManager           Resources;
	FSceneRenderer             SceneRenderer;
	FImGuiLayer                ImGuiLayer;
	FScene                     Scene;

	FCamera              Camera;
	FFlyCameraController CameraController;

	FEditorContext       Context;
	FViewportPanel       ViewportPanel;
	FHierarchyPanel      HierarchyPanel;
	FInspectorPanel      InspectorPanel;
	FContentBrowserPanel ContentBrowserPanel;

	std::filesystem::path CurrentScenePath; // 비어 있으면 저장된 적 없는 씬

	bool  bShowStats     = true;
	bool  bShowImGuiDemo = false;
	float SmoothedFps    = 0.0f;
};

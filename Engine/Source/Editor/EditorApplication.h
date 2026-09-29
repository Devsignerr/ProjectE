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

#include <memory>

class FD3D12RHI;

// 에디터 애플리케이션: RHI/리소스/씬/렌더러 + ImGui 패널
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
	void BuildDefaultScene();
	void DrawMainMenuBar();
	void DrawStatsWindow();

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

	bool  bShowStats     = true;
	bool  bShowImGuiDemo = false;
	float SmoothedFps    = 0.0f;
};

#pragma once

#include "Core/Application.h"
#include "Core/FileWatcher.h"
#include "Editor/EditorContext.h"
#include "Editor/ImGuiLayer.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/PostProcessPanel.h"
#include "Editor/Panels/ShadowPanel.h"
#include "Editor/Panels/OutputLogPanel.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ViewportPanel.h"
#include "Editor/PlayMode.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

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
	void OnScreenshotRequested(const std::filesystem::path& Path) override;

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

	// ---- 셰이더 핫 리로드 (개발 기능)
	void PollShaderChanges();                         // 셰이더 디렉터리 변경 감지 → 무효화 → PSO 재생성
	void ReloadAllShaders();                          // 강제 전체 재컴파일 (Ctrl+R)
	void HandleToolShortcuts();
	void DrawToolsMenu();
	void DrawNotification();
	void ShowNotification(std::string Message, bool bError);

	// ---- 플레이 모드 / 스크립트
	void StartPlay();
	void StopPlay();
	void UpdatePlayMode(float DeltaSeconds);
	void HandlePlayShortcuts();
	void DrawPlayControls(); // 메인 메뉴 바 안의 재생/일시정지/진행/정지 버튼
	void PollScriptChanges(); // Content의 .lua 저장 감지 → 핫 리로드

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
	FPostProcessPanel    PostProcessPanel;
	FShadowPanel         ShadowPanel;
	FOutputLogPanel      OutputLogPanel;

	std::filesystem::path CurrentScenePath; // 비어 있으면 저장된 적 없는 씬

	FScriptSystem Scripts;
	FPlayMode     PlayMode;
	FFileWatcher  ScriptWatcher;

	FFileWatcher                          ShaderWatcher;
	std::string                           NotificationText;
	bool                                  bNotificationError = false;
	std::chrono::steady_clock::time_point NotificationExpiry;

	bool  bShowStats     = true;
	bool  bShowImGuiDemo = false;
	float SmoothedFps    = 0.0f;
};

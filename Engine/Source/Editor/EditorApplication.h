#pragma once

#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Core/Application.h"
#include "Core/FileWatcher.h"
#include "Editor/AssetEditors/AssetEditorManager.h"
#include "Editor/ContentBrowser/AssetReferenceUpdater.h"
#include "Editor/EditorContext.h"
#include "Editor/ImGuiLayer.h"
#include "Editor/ModelTemplateCache.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/PostProcessPanel.h"
#include "Editor/Panels/ShadowPanel.h"
#include "Editor/Panels/OutputLogPanel.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ViewportPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/UndoHistory.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/GameModuleHost.h"
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
	void ApplyDefaultLayoutIfNeeded(); // 첫 실행 / 메뉴 요청 시 언리얼 풍 기본 도킹 배치
	void OnAssetsMoved(const std::vector<FAssetMove>& Moves); // 콘텐츠 브라우저 이동/이름 변경 후 열린 씬·기록·캐시 갱신
	void VerifyAssetMove();                                   // 자동 검증 --verify-asset-move
	bool ReimportModelAsset(const std::filesystem::path& Path); // 임포트 설정 적용: 캐시 교체 + 열린 씬/편집 창/썸네일 갱신
	void VerifyReimport(const std::filesystem::path& ModelPath); // 자동 검증 --verify-reimport

	// ---- 실행 취소 (씬 스냅샷)
	void ResetUndoHistory();                       // 씬 열기/새 씬 직후 기준 상태로
	void CommitPendingEdit();                      // 조작이 끝난 편집을 Undo 단계로 기록
	void UndoEdit();
	void RedoEdit();
	void RestoreSnapshot(const std::string& State); // 선택은 엔티티 경로로 복구
	void DrawEditMenu();

	// ---- 에디터 카메라 저장/복원 (Saved/EditorCamera.json)
	void LoadEditorCamera();
	void SaveEditorCamera() const;

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
	FAssetEditorManager  AssetEditors; // 머티리얼/메시/애니메이션/파티클 편집 창

	std::filesystem::path CurrentScenePath; // 비어 있으면 저장된 적 없는 씬

	FUndoHistory        UndoHistory;
	FModelTemplateCache ModelTemplates; // Undo 복원 시 모델 하위 노드 재사용

	FScriptSystem Scripts;
	FPlayMode     PlayMode;
	FFileWatcher  ScriptWatcher;

	// 오디오는 플레이 중에만 재생 (정지 시 모든 소스 해제)
	FAudioEngine Audio;
	FAudioSystem AudioSystem;

	// 물리도 플레이 중에만 (FPlayMode가 Begin/Update/End)
	FPhysicsSystem Physics;

	// 프로젝트 C++ 게임 모듈: 타입은 시작 시 등록, 시스템은 플레이 중에만 (FPlayMode)
	FGameModuleHost GameModule;

	FFileWatcher                          ShaderWatcher;
	std::string                           NotificationText;
	bool                                  bNotificationError = false;
	std::chrono::steady_clock::time_point NotificationExpiry;

	bool  bShowStats     = true;
	bool  bShowImGuiDemo = false;
	bool  bLayoutChecked        = false;
	bool  bResetLayoutRequested = false;
	float SmoothedFps    = 0.0f;
};

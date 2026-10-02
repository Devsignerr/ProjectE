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
#include "Editor/Panels/NetworkPanel.h"
#include "Editor/Panels/PostProcessPanel.h"
#include "Editor/Panels/SettingsWindow.h"
#include "Editor/Panels/ShadowPanel.h"
#include "Editor/Panels/FoliageToolPanel.h"
#include "Editor/Panels/TerrainToolPanel.h"
#include "Editor/Panels/OutputLogPanel.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ViewportPanel.h"
#include "Editor/PlayInEditorNet.h"
#include "Editor/PlayMode.h"
#include "Editor/UndoHistory.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StatOverlay.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

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
	void OpenStartupScene(); // --scene → 마지막 씬(환경설정) → 프로젝트 설정 "에디터 시작 맵" (없으면 기본 씬 구성 후 생성)
	void RememberOpenedScene(); // 프로젝트별 개인 상태에 마지막 씬 기록
	void UpdateAutoSave(float DeltaSeconds); // 환경설정 "자동 저장": 저장 안 한 변경을 <Saved>/Autosaves/에 사본으로
	void ApplyViewportPreferences(); // 환경설정 "뷰포트" → 카메라 감도/시야각, 스냅
	void UpdateWindowTitle();

	void BuildDefaultScene();
	void DrawMainMenuBar();
	void DrawStatsWindow();
	void HandleShortcuts();
	void ApplyDefaultLayoutIfNeeded(); // 첫 실행 / 메뉴 요청 시 언리얼 풍 기본 도킹 배치
	void OnAssetsMoved(const std::vector<FAssetMove>& Moves); // 콘텐츠 브라우저 이동/이름 변경 후 열린 씬·기록·캐시 갱신
	void VerifyAssetMove();                                   // 자동 검증 --verify-asset-move
	void VerifyPick(const std::string& Targets);              // 자동 검증 --verify-pick <이름>[,<이름>...] [--verify-pick-ortho] [--verify-pick-no-focus]
	void VerifyTerrainBrush();                                // 자동 검증 --terrain-brush-test (지형 브러시 + Undo/Redo)
	void VerifyFoliageBrush();                                // 자동 검증 --foliage-brush-test (폴리지 칠하기/지우기 + Undo/Redo)
	bool ReimportModelAsset(const std::filesystem::path& Path); // 임포트 설정 적용: 캐시 교체 + 열린 씬/편집 창/썸네일 갱신
	void VerifyReimport(const std::filesystem::path& ModelPath); // 자동 검증 --verify-reimport
	// 프리팹 원본을 바꾸는 작업 감싸기: 오버라이드 기록(옛 원본) → Change → 캐시 비우기 → 열린 씬 인스턴스 동기화 + 에셋 해석
	bool ChangePrefabAsset(const std::function<bool()>& Change);

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
	// 도구 → 내비메시 굽기: 편집 씬 정적 메시 → 씬 옆 .enav + NavMeshComponent 지정 + 뷰포트 표시
	void BakeNavMesh();
	void SyncNavMeshDisplay(); // 편집 씬 NavMeshComponent 파일이 바뀌면 뷰포트 표시를 다시 읽는다 (매 프레임, 바뀔 때만 로드)
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
	FTerrainToolPanel    TerrainToolPanel; // 지형 스컬프트/칠하기 (Phase 34)
	FFoliageToolPanel    FoliageToolPanel; // 풀·나무 칠하기 (Phase 34-3)
	FSettingsWindow      ProjectSettingsWindow{ FSettingsWindow::EKind::ProjectSettings };     // 편집 → 프로젝트 설정
	FSettingsWindow      EditorPreferencesWindow{ FSettingsWindow::EKind::EditorPreferences }; // 편집 → 에디터 환경설정
	FOutputLogPanel      OutputLogPanel;
	FNetworkPanel        NetworkPanel;
	FAssetEditorManager  AssetEditors; // 머티리얼/메시/애니메이션/파티클 편집 창

	std::filesystem::path CurrentScenePath; // 비어 있으면 저장된 적 없는 씬
	std::string           DisplayedNavMeshAsset; // 뷰포트에 표시 중인 내비메시 파일 (Content 기준)

	FUndoHistory        UndoHistory;
	FModelTemplateCache ModelTemplates; // Undo 복원 시 모델 하위 노드 재사용

	FScriptSystem Scripts;
	FPlayMode     PlayMode;
	FFileWatcher  ScriptWatcher;

	// 오디오는 플레이 중에만 재생 (정지 시 모든 소스 해제)
	FAudioEngine Audio;
	FAudioSystem AudioSystem;

	// 물리도 플레이 중에만 (FGameWorld가 Begin/Update/End)
	FPhysicsSystem Physics;

	// 프로젝트 C++ 게임 모듈: 타입은 시작 시 등록, 시스템은 플레이 중에만 (FGameWorld)
	FGameModuleHost GameModule;

	// 게임 월드 갱신 순서 (플레이 중 게임플레이 틱 + 항상 표시 틱). 위 시스템들을 비소유로 참조
	FGameWorld World;

	// 네트워크 플레이 (리슨/전용 서버 + 런타임 클라이언트 창). 설정은 네트워크 패널
	FPlayInEditorNet NetPlay;

	FFileWatcher                          ShaderWatcher;
	std::string                           NotificationText;
	bool                                  bNotificationError = false;
	std::chrono::steady_clock::time_point NotificationExpiry;

	bool  bShowStats     = true;
	bool  bShowImGuiDemo = false;
	bool  bLayoutChecked        = false;
	bool  bResetLayoutRequested = false;
	float  VerifyCameraPanPerFrame = 0.0f; // --verify-camera-pan <cm/프레임>: 편집 카메라를 오른쪽으로 일정하게 민다 (움직일 때 시간 떨림 확인)
	uint64 VerifyCameraPanStart    = 0;    // --verify-camera-pan-start <프레임>: 이 프레임부터 민다
	bool  bScriptStopPlayRequested = false; // Lua Game.Quit() → 이번 플레이 틱이 끝난 뒤 정지
	float AutoSaveElapsedSeconds   = 0.0f;
	float SmoothedFps    = 0.0f;
	FStatOverlay StatOverlay; // 콘솔 stat fps/gpu → 뷰포트 오른쪽 위
};

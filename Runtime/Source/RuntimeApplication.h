#pragma once

#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Core/Application.h"
#include "Core/GameUserSettings.h"
#include "Core/InputMode.h"
#include "Network/LanDiscovery.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/Camera.h"
#include "Renderer/DebugDraw.h"
#include "Renderer/DebugDrawRenderer.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/RenderThread.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StatOverlay.h"
#include "Renderer/UIRenderer.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "UI/UIConsoleOverlay.h"
#include "UI/UIDrawList.h"
#include "World/GameWorld.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class FConsoleVariable;
class FD3D12RHI;

// 게임 런타임: 프로젝트를 열어 씬을 렌더링한다 (에디터 UI 없음)
class FRuntimeApplication final : public FApplication
{
public:
	FRuntimeApplication();
	~FRuntimeApplication() override; // unique_ptr<FD3D12RHI> 완전 타입이 필요하므로 cpp에 정의

protected:
	void OnConfigureWindow(FWindowDesc& WindowDesc) override;
	bool OnInit() override;
	void OnUpdate(float DeltaSeconds) override;
	void OnRender() override;
	void OnResize(uint32 Width, uint32 Height) override;
	void OnShutdown() override;
	void OnScreenshotRequested(const std::filesystem::path& Path) override;

private:
	void BuildPlaceholderScene();

	// ---- 화면 설정 (창/테두리 없는 전체 화면, VSync). 자동 검증 실행에서는 사용자 설정 파일을 읽거나 쓰지 않는다
	void ApplyWindowMode(EWindowMode Mode, bool bSave);
	void SetVSync(bool bEnabled);
	void SaveUserSettings() const;
	// 입력 모드 (Core/InputMode.h): 모드가 바뀌면 커서 기본값(GameOnly = 잠금)을 적용하고, GameOnly에서 잠금이 풀렸으면 클릭으로 다시 잠근다
	void UpdateInputModeCursor(const FInput& InputState);

	// ---- 세션 (RuntimeSession.cpp): 씬 로드 + 넷 모드별 시작/종료, 스크립트 요청(Net.Host/Connect/Disconnect) 처리
	void LoadScene(); // SceneAsset (없거나 실패하면 자리표시 씬)
	void StartSession(FNetLaunchOptions Options);
	void EndSession();
	void HandleSessionRequest(const FNetSessionRequest& Request);
	void StartListenServer(uint16 Port); // 이미 World가 Standalone으로 도는 상태에서
	void StartLanHost();                 // 리슨 서버 LAN 알림 (지금 씬으로)
	void UpdateWindowTitle();
	// 맵 전환 (Game.OpenScene / 서버 지시): 요청 프레임은 검은 화면만 그리고, 다음 프레임 처음에 FGameWorldTravel::Travel
	void TravelTo(const std::string& NextScene);

	// ---- 렌더 스레드 (r.RenderThread — Renderer/RenderThread.h 머리 주석): OnRender = 게임 스레드 준비(BeginFrame·씬 수집·패스 등록·
	// 오버레이 입력 사본) + 렌더 작업(그래프 실행·오버레이 기록·EndFrame/Present). r.RenderThread 0이면 렌더 작업을 그 자리에서 돈다
	void RecordAndPresent(const std::filesystem::path& Screenshot, const std::filesystem::path& ContentDirectory);
	void LogRenderThreadStats() const;

	std::string                SceneAsset;    // Content 기준 현재 씬
	std::optional<std::string> PendingTravel; // 다음 프레임에 열 씬 (이번 프레임은 로딩 화면)
	uint16                     HostPort = 0;  // 리슨 서버 포트 (LAN 알림)
	uint32                     AppliedInputModeRevision = 0; // 커서 기본값을 마지막으로 적용한 FInputModeState 리비전
	EInputMode                 AppliedInputMode         = EInputMode::GameAndUI;

	FGameUserSettings UserSettings;

	std::unique_ptr<FD3D12RHI> Rhi;
	FResourceManager           Resources;
	FSceneRenderer             SceneRenderer;
	FDebugDrawRenderer         DebugDrawRenderer; // 3D 디버그 선 (씬 위, 게임 UI 아래 — Renderer/DebugDraw.h)
	FUIRenderer                UIRenderer; // 게임 UI (씬 위)
	FUIDrawList                UIDrawList; // 프레임마다 다시 채움
	FUIConsoleOverlay          Console;     // ` 키 개발자 콘솔 (게임 UI 위, 패키지 게임은 프로젝트 설정 Console.EnableInPackagedGame)
	FStatOverlay               StatOverlay; // 콘솔 stat fps/gpu → 오른쪽 위
	FScene                     Scene;

	FCamera              Camera;
	FFlyCameraController CameraController;

	FRenderThread           RenderThread;
	FConsoleVariable*       RenderThreadVar = nullptr; // r.RenderThread (엔진 DLL 밖이라 이름으로 찾는다)
	// 렌더 작업이 읽는 이번 프레임 사본 (게임 스레드가 WaitIdle 뒤에만 쓴다)
	FCamera                 RenderCamera;
	FRenderOutput           RenderSceneOutput;
	std::vector<FDebugLine> RenderDebugLines;
	std::filesystem::path   PendingScreenshot; // OnScreenshotRequested → 이번 프레임 EndFrame 직전
	// CPU 구간 측정 (종료 로그 [성능] 렌더 스레드): 게임 스레드 작업(렌더 작업·대기 제외), 렌더 작업의 기록(FinishRender + 오버레이)과
	// 제출(EndFrame — GPU가 밀리면 Present 대기 포함). 0이면 프레임 CPU ≈ 게임 + 기록 + 제출, 1이면 ≈ max(게임, 기록 + 제출)
	struct FCpuFrameTimes
	{
		double                                GameMs   = 0.0;
		double                                RecordMs = 0.0; // 렌더 작업만 쓴다 (종료 때 Stop 뒤에 읽는다)
		double                                SubmitMs = 0.0;
		double                                BeginFrameMs = 0.0; // 게임 스레드 BeginFrame (GPU가 밀리면 슬롯 펜스 대기 — 게임 스레드에 포함)
		uint64                                Frames   = 0;
		std::chrono::steady_clock::time_point LastKickEnd;
		bool                                  bHasLastKick = false;
	};
	FCpuFrameTimes CpuTimes;

	FScriptSystem        Scripts; // 씬의 스크립트 컴포넌트 실행 (로드 직후 BeginPlay)
	FGameModuleHost      GameModule; // 프로젝트 C++ 게임 모듈 (있으면)
	FAudioEngine         Audio;
	FAudioSystem         AudioSystem;
	FPhysicsSystem       Physics; // 항상 시뮬레이션 (씬 로드 후 Begin)
	FGameWorld           World;   // 게임 월드 갱신 순서 (위 시스템들을 비소유로 참조)
	FNetDriver           Net;     // --host(리슨 서버) / --connect(클라이언트). 없으면 Standalone
	FReplicationServer   ReplicationServer; // 리슨 서버: 복제 엔티티 → 클라이언트
	FReplicationClient   ReplicationClient; // 클라이언트: 서버 상태 적용 (게임 로직은 돌리지 않는다)
	FNetPlayerSpawner    Players;           // 리슨 서버: 입장/퇴장 → PlayerPrefab 생성/제거 (호스트 포함)
	FLanDiscovery        Lan;               // 리슨 서버: LAN 방 알림 / --join-lan: 세션 찾기
};

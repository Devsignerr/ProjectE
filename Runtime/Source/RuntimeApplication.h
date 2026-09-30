#pragma once

#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Core/Application.h"
#include "Network/LanDiscovery.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationClient.h"
#include "Network/ReplicationServer.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <memory>

class FD3D12RHI;

// 게임 런타임: 프로젝트를 열어 씬을 렌더링한다 (에디터 UI 없음)
class FRuntimeApplication final : public FApplication
{
public:
	FRuntimeApplication();
	~FRuntimeApplication() override; // unique_ptr<FD3D12RHI> 완전 타입이 필요하므로 cpp에 정의

protected:
	bool OnInit() override;
	void OnUpdate(float DeltaSeconds) override;
	void OnRender() override;
	void OnResize(uint32 Width, uint32 Height) override;
	void OnShutdown() override;
	void OnScreenshotRequested(const std::filesystem::path& Path) override;

private:
	void BuildPlaceholderScene();

	// ---- 세션 (RuntimeSession.cpp): 씬 로드 + 넷 모드별 시작/종료, 스크립트 요청(Net.Host/Connect/Disconnect) 처리
	void LoadScene(); // SceneAsset (없거나 실패하면 자리표시 씬)
	void StartSession(FNetLaunchOptions Options);
	void EndSession();
	void HandleSessionRequest(const FNetSessionRequest& Request);
	void StartListenServer(uint16 Port); // 이미 World가 Standalone으로 도는 상태에서
	void UpdateWindowTitle();

	std::string SceneAsset; // Content 기준 현재 씬

	std::unique_ptr<FD3D12RHI> Rhi;
	FResourceManager           Resources;
	FSceneRenderer             SceneRenderer;
	FScene                     Scene;

	FCamera              Camera;
	FFlyCameraController CameraController;

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

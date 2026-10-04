#pragma once

#include "Core/Application.h"
#include "Network/LanDiscovery.h"
#include "Network/NetDriver.h"
#include "Network/NetPlayerSpawner.h"
#include "Network/ReplicationServer.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

// 전용 서버: 창·GPU 없이 프로젝트 씬을 로드해 게임 월드(스크립트/게임 모듈/물리/애니메이션)를 고정 틱으로 돌린다.
// 인자: --project <경로> --scene <Content 기준 경로> (없으면 프로젝트 기본 씬), --port <포트> (기본 7777), --exit-after <틱 수>, --log <경로>,
//   --exit-when-empty <틱 수> [--exit-min-players <수>(기본 1)] (자동 검증: 그 수만큼 입장한 뒤 모두 나가면 그 틱 수 뒤 정상 종료 — Verify.ps1 -Multiplayer)
// 알려진 제한: GPU 리소스가 없어 에셋 핸들 해석(FSceneAssetResolver)을 하지 않는다 → 모델 하위 노드(뼈대)가 생기지 않는다
class FServerApplication final : public FApplication
{
public:
	FServerApplication();

protected:
	bool OnInit() override;
	void OnUpdate(float DeltaSeconds) override;
	void OnShutdown() override;

private:
	void StartLanHost(const std::string& SceneAsset); // LAN 방 알림 (맵 전환 후 다시)

	uint16          Port = 0;
	int64           ExitWhenEmptyTicks = -1; // --exit-when-empty (-1 = 끔)
	int64           EmptyTicks         = 0;
	int64           ExitMinPlayers     = 1;
	int64           JoinCount          = 0; // 입장 누적 (나간 플레이어 포함)
	FScene          Scene;
	FScriptSystem   Scripts;
	FGameModuleHost GameModule;
	FPhysicsSystem  Physics;
	FGameWorld      World; // 위 시스템들을 비소유로 참조
	FNetDriver         Net;         // 전용 서버 연결 관리 (접속/핸드셰이크/플레이어 목록)
	FReplicationServer Replication; // 복제 엔티티 → 클라이언트 (게임플레이 틱 뒤)
	FNetPlayerSpawner  Players;     // 입장/퇴장 → .eproject PlayerPrefab 생성/제거
	FLanDiscovery      Lan;         // LAN 방 목록에 알림 (탐색 포트 7778)
};

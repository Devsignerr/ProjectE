#pragma once

#include "Core/Application.h"
#include "Network/NetDriver.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/GameModuleHost.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

// 전용 서버: 창·GPU 없이 프로젝트 씬을 로드해 게임 월드(스크립트/게임 모듈/물리/애니메이션)를 고정 틱으로 돌린다.
// 인자: --project <경로> --scene <Content 기준 경로> (없으면 프로젝트 기본 씬), --port <포트> (기본 7777), --exit-after <틱 수>, --log <경로>
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
	FScene          Scene;
	FScriptSystem   Scripts;
	FGameModuleHost GameModule;
	FPhysicsSystem  Physics;
	FGameWorld      World; // 위 시스템들을 비소유로 참조
	FNetDriver      Net;   // 전용 서버 연결 관리 (접속/핸드셰이크/플레이어 목록)
};

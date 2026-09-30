#pragma once

#include "Audio/AudioEngine.h"
#include "Audio/AudioSystem.h"
#include "Core/Application.h"
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
};

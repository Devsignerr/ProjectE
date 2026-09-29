#pragma once

#include "Core/Application.h"
#include "Renderer/Camera.h"
#include "Renderer/FlyCameraController.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

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

private:
	void BuildPlaceholderScene();

	std::unique_ptr<FD3D12RHI> Rhi;
	FResourceManager           Resources;
	FSceneRenderer             SceneRenderer;
	FScene                     Scene;

	FCamera              Camera;
	FFlyCameraController CameraController;
};

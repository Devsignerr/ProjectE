#pragma once

#include "Core/ECS/Entity.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FSceneRenderer;

// 패널들이 공유하는 에디터 상태 (소유하지 않음)
struct FEditorContext
{
	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;
	FSceneRenderer*   Renderer  = nullptr;
	FScene*           Scene     = nullptr;
	FCamera*          Camera    = nullptr;

	FEntity SelectedEntity;

	std::filesystem::path ContentDirectory;
	FMeshHandle           DefaultCubeMesh; // "큐브 추가" 등에 사용

	void Select(FEntity Entity) { SelectedEntity = Entity; }
	void ClearSelection() { SelectedEntity = NullEntity; }
};

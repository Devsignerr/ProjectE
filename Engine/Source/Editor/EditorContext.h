#pragma once

#include "Core/ECS/Entity.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>
#include <functional>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FSceneRenderer;
class FScriptSystem;

// 패널들이 공유하는 에디터 상태 (소유하지 않음)
struct FEditorContext
{
	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;
	FSceneRenderer*   Renderer  = nullptr;
	FScene*           Scene     = nullptr;
	FCamera*          Camera    = nullptr;
	FScriptSystem*    Scripts   = nullptr; // 스크립트 Properties 선언 조회 (인스펙터)

	FEntity SelectedEntity;

	// 플레이 모드 (FPlayMode가 갱신). 플레이 중 Scene은 복제된 플레이 씬을 가리킨다
	bool bPlaying = false;
	bool bPaused  = false;

	std::filesystem::path ContentDirectory;
	FMeshHandle           DefaultCubeMesh; // "큐브 추가" 등에 사용 (MeshAsset "primitive:cube")

	// 패널 → 애플리케이션 요청 (씬 파일 열기 등)
	std::function<void(const std::filesystem::path&)> OpenSceneRequest;

	void Select(FEntity Entity) { SelectedEntity = Entity; }
	void ClearSelection() { SelectedEntity = NullEntity; }
};

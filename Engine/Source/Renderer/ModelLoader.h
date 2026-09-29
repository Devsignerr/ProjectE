#pragma once

#include "Core/ECS/Entity.h"
#include "Renderer/GltfLoader.h"

#include <filesystem>

class FResourceManager;
class FScene;

// 모델 데이터를 GPU 리소스로 올리고 씬에 엔티티 계층으로 배치한다
struct FModelLoader
{
	// glTF/GLB 로드 + 씬 배치. 반환: 모델 루트 엔티티 (실패 시 NullEntity)
	static FEntity LoadIntoScene(const std::filesystem::path& Path, FScene& Scene, FResourceManager& Resources,
	                             FEntity Parent = NullEntity);

	static FEntity Instantiate(const FModelData& Model, FScene& Scene, FResourceManager& Resources, FEntity Parent = NullEntity);
};

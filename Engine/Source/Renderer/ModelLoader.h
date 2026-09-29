#pragma once

#include "Core/ECS/Entity.h"
#include "Renderer/GltfLoader.h"

#include <filesystem>

class FResourceManager;
class FScene;

// 모델 데이터를 GPU 리소스로 올리고 씬에 엔티티 계층으로 배치한다.
// 루트 엔티티에는 FModelComponent(에셋 경로)가, 생성된 하위 노드에는 FTransientComponent가 붙는다.
struct FModelLoader
{
	// glTF/GLB 로드 + 새 루트 엔티티 생성. 반환: 루트 (실패 시 NullEntity)
	static FEntity LoadIntoScene(const std::filesystem::path& Path, FScene& Scene, FResourceManager& Resources,
	                             FEntity Parent = NullEntity);

	// 기존 엔티티를 루트로 사용해 하위 노드 생성 (씬 로드 후 복원용)
	static bool LoadIntoEntity(const std::filesystem::path& Path, FScene& Scene, FResourceManager& Resources, FEntity Root);

	static FEntity Instantiate(const FModelData& Model, FScene& Scene, FResourceManager& Resources, FEntity Parent = NullEntity);
	static void    InstantiateInto(const FModelData& Model, FScene& Scene, FResourceManager& Resources, FEntity Root);

	// 프로젝트 Content 안의 경로면 Content 기준 상대 경로('/' 구분), 아니면 절대 경로 문자열
	static std::string MakeAssetPath(const std::filesystem::path& Path);
};

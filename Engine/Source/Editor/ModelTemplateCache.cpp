#include "Editor/ModelTemplateCache.h"

#include "Editor/SceneEditOps.h"
#include "Renderer/ResourceCollector.h"

#include <vector>

void FModelTemplateCache::Capture(FScene& Scene)
{
	// 순회 중 다른 씬(TemplateScene)만 변경하므로 안전하지만, 모델 목록을 먼저 모아 둔다
	std::vector<std::pair<FEntity, std::string>> Models;
	Scene.GetRegistry().View<FModelComponent>().Each([&](FEntity Entity, FModelComponent& Model) {
		if (!Model.AssetPath.empty() && !Scene.GetChildren(Entity).empty() && !Templates.contains(Model.AssetPath))
		{
			Models.emplace_back(Entity, Model.AssetPath);
		}
	});

	for (const auto& [Entity, AssetPath] : Models)
	{
		if (!Templates.contains(AssetPath))
		{
			Templates[AssetPath] = FSceneEditOps::CloneSubtree(Scene, Entity, TemplateScene, NullEntity);
		}
	}
}

uint32 FModelTemplateCache::Instantiate(FScene& Scene)
{
	std::vector<std::pair<FEntity, FEntity>> Targets; // (씬 모델 루트, 템플릿 루트)
	Scene.GetRegistry().View<FModelComponent>().Each([&](FEntity Entity, FModelComponent& Model) {
		const auto Found = Templates.find(Model.AssetPath);
		if (Found != Templates.end() && Scene.GetChildren(Entity).empty())
		{
			Targets.emplace_back(Entity, Found->second);
		}
	});

	for (const auto& [Root, Template] : Targets)
	{
		FSceneEditOps::CloneChildren(TemplateScene, Template, Scene, Root);
	}
	return static_cast<uint32>(Targets.size());
}

void FModelTemplateCache::Clear()
{
	TemplateScene.Clear();
	Templates.clear();
}

void FModelTemplateCache::CollectResourceRoots(FResourceRoots& Roots)
{
	Roots.AddScene(TemplateScene);
}

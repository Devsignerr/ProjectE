#include "Renderer/SceneAssetResolver.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr std::string_view GPrimitivePrefix = "primitive:";

	std::filesystem::path ResolveContentPath(const std::string& AssetPath, const std::filesystem::path& ContentDirectory)
	{
		const std::filesystem::path Path = FStringConv::ToWide(AssetPath);
		return Path.is_absolute() ? Path : ContentDirectory / Path;
	}
} // namespace

void FSceneAssetResolver::Resolve(FScene& Scene, FResourceManager& Resources, const std::filesystem::path& ContentDirectory)
{
	FRegistry& Registry = Scene.GetRegistry();

	// 정적 메시: 내장 도형 + 머티리얼 에셋
	Registry.View<FStaticMeshComponent>().Each([&](FEntity, FStaticMeshComponent& Mesh) {
		if (!Mesh.Mesh.IsValid() && !Mesh.MeshAsset.empty())
		{
			if (Mesh.MeshAsset.rfind(GPrimitivePrefix, 0) == 0)
			{
				Mesh.Mesh = Resources.GetOrCreatePrimitiveMesh(std::string_view(Mesh.MeshAsset).substr(GPrimitivePrefix.size()));
			}
			else
			{
				E_LOG(LogRenderer, Warning, "지원하지 않는 메시 에셋 참조: {} (파일 메시는 에셋 파이프라인에서 지원 예정)", Mesh.MeshAsset);
			}
		}
		if (!Mesh.Material.IsValid() && !Mesh.MaterialAsset.empty())
		{
			Mesh.Material = Resources.LoadMaterial(ResolveContentPath(Mesh.MaterialAsset, ContentDirectory));
		}
	});

	// 모델: 자식이 없는 루트만 다시 인스턴스화 (순회 중 엔티티 생성 금지 → 먼저 수집)
	std::vector<std::pair<FEntity, std::string>> PendingModels;
	Registry.View<FModelComponent>().Each([&](FEntity Entity, FModelComponent& Model) {
		if (Scene.GetChildren(Entity).empty() && !Model.AssetPath.empty())
		{
			PendingModels.emplace_back(Entity, Model.AssetPath);
		}
	});
	for (const auto& [Entity, AssetPath] : PendingModels)
	{
		if (!FModelLoader::LoadIntoEntity(ResolveContentPath(AssetPath, ContentDirectory), Scene, Resources, Entity))
		{
			E_LOG(LogRenderer, Warning, "모델 에셋을 복원하지 못했습니다: {}", AssetPath);
		}
	}

	ResolveParticles(Scene, Resources, ContentDirectory);

	Scene.UpdateTransforms();
}

void FSceneAssetResolver::ResolveParticles(FScene& Scene, FResourceManager& Resources, const std::filesystem::path& ContentDirectory)
{
	Scene.GetRegistry().View<FParticleSystemComponent>().Each([&](FEntity, FParticleSystemComponent& Emitter) {
		FParticleRuntime& Runtime = Emitter.Runtime;
		// 같은 경로는 다시 해석하지 않는다 (실패한 경로도 매 프레임 재시도하지 않음 — 경로를 바꾸면 다시 해석)
		if (Runtime.ResolvedAsset == Emitter.Asset)
		{
			return;
		}
		Runtime.Restart();
		Runtime.ResolvedAsset = Emitter.Asset;
		Runtime.System        = Emitter.Asset.empty() ? nullptr : Resources.LoadParticleSystem(ResolveContentPath(Emitter.Asset, ContentDirectory));
		if (!Emitter.Asset.empty() && !Runtime.System)
		{
			E_LOG(LogRenderer, Warning, "파티클 에셋을 불러오지 못했습니다: {}", Emitter.Asset);
		}
	});
}

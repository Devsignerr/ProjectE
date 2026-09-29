#include "Renderer/ModelLoader.h"

#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "Renderer/AssetCache.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Scene.h"

#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	void InstantiateNode(const FModelData& Model, int32 NodeIndex, FEntity ParentEntity, FScene& Scene,
	                     const std::vector<FMeshHandle>& MeshHandles, const std::vector<FMaterialHandle>& MaterialHandles,
	                     FMaterialHandle DefaultMaterial)
	{
		const FModelNode& Node   = Model.Nodes[NodeIndex];
		const FEntity     Entity = Scene.CreateEntity(Node.Name);
		Scene.SetParent(Entity, ParentEntity);
		Scene.GetRegistry().Emplace<FTransientComponent>(Entity); // 모델에서 생성된 노드: 직렬화 제외

		FTransformComponent& Transform = Scene.GetTransform(Entity);
		Transform.Position = Node.Translation;
		Transform.Rotation = Node.Rotation;
		Transform.Scale    = Node.Scale;

		auto AttachMesh = [&](FEntity Target, int32 MeshIndex) {
			const FModelMesh& Mesh = Model.Meshes[MeshIndex];
			if (!MeshHandles[MeshIndex].IsValid())
			{
				return;
			}
			FStaticMeshComponent& Component = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Target);
			Component.Mesh     = MeshHandles[MeshIndex];
			Component.Material = (Mesh.Material >= 0 && MaterialHandles[Mesh.Material].IsValid()) ? MaterialHandles[Mesh.Material] : DefaultMaterial;
		};

		// primitive가 하나면 노드 자체에, 여러 개면 자식 엔티티에 부착
		if (Node.Meshes.size() == 1)
		{
			AttachMesh(Entity, Node.Meshes[0]);
		}
		else
		{
			for (size_t Index = 0; Index < Node.Meshes.size(); ++Index)
			{
				const FEntity MeshEntity = Scene.CreateEntity(std::format("{}_Primitive{}", Node.Name, Index));
				Scene.SetParent(MeshEntity, Entity);
				Scene.GetRegistry().Emplace<FTransientComponent>(MeshEntity);
				AttachMesh(MeshEntity, Node.Meshes[Index]);
			}
		}

		for (int32 Child : Node.Children)
		{
			InstantiateNode(Model, Child, Entity, Scene, MeshHandles, MaterialHandles, DefaultMaterial);
		}
	}
} // namespace

std::string FModelLoader::MakeAssetPath(const std::filesystem::path& Path)
{
	std::error_code ErrorCode;
	if (FPaths::IsInitialized() && FPaths::HasProject())
	{
		const std::filesystem::path Relative = std::filesystem::relative(Path, FPaths::GetProjectContentDirectory(), ErrorCode);
		if (!ErrorCode && !Relative.empty() && Relative.native().rfind(L"..", 0) != 0)
		{
			return FStringConv::ToUtf8(Relative.generic_wstring());
		}
	}
	return FStringConv::ToUtf8(Path.generic_wstring());
}

FEntity FModelLoader::LoadIntoScene(const std::filesystem::path& Path, FScene& Scene, FResourceManager& Resources, FEntity Parent)
{
	FModelData Model;
	if (FAssetCache::LoadModelAsset(Path, Model) == FAssetCache::ESource::Failed)
	{
		return NullEntity;
	}

	const FEntity Root = Scene.CreateEntity(Model.Name);
	Scene.SetParent(Root, Parent);
	Scene.GetRegistry().Emplace<FModelComponent>(Root).AssetPath = MakeAssetPath(Path);
	InstantiateInto(Model, Scene, Resources, Root);
	return Root;
}

bool FModelLoader::LoadIntoEntity(const std::filesystem::path& Path, FScene& Scene, FResourceManager& Resources, FEntity Root)
{
	FModelData Model;
	if (FAssetCache::LoadModelAsset(Path, Model) == FAssetCache::ESource::Failed)
	{
		return false;
	}
	InstantiateInto(Model, Scene, Resources, Root);
	return true;
}

FEntity FModelLoader::Instantiate(const FModelData& Model, FScene& Scene, FResourceManager& Resources, FEntity Parent)
{
	const FEntity Root = Scene.CreateEntity(Model.Name);
	Scene.SetParent(Root, Parent);
	InstantiateInto(Model, Scene, Resources, Root);
	return Root;
}

void FModelLoader::InstantiateInto(const FModelData& Model, FScene& Scene, FResourceManager& Resources, FEntity Root)
{
	// 텍스처: 베이스 컬러로 참조되는 이미지만 sRGB로 업로드
	std::unordered_map<int32, FTextureHandle> ImageTextures;
	auto GetOrCreateTexture = [&](int32 ImageIndex) -> FTextureHandle {
		if (ImageIndex < 0 || ImageIndex >= static_cast<int32>(Model.Images.size()))
		{
			return FTextureHandle{};
		}
		if (const auto Found = ImageTextures.find(ImageIndex); Found != ImageTextures.end())
		{
			return Found->second;
		}
		const FModelImage& Image = Model.Images[ImageIndex];
		FTextureHandle     Handle;
		if (Image.Image.IsValid())
		{
			Handle = Resources.CreateTexture(Image.Image, true, FStringConv::ToWide(Model.Name + "/" + Image.Name));
		}
		ImageTextures[ImageIndex] = Handle;
		return Handle;
	};

	// 머티리얼 (PBR 인자를 Blinn-Phong으로 근사; Phase 6 PBR에서 교체)
	std::vector<FMaterialHandle> MaterialHandles(Model.Materials.size());
	for (size_t Index = 0; Index < Model.Materials.size(); ++Index)
	{
		const FModelMaterial& Source = Model.Materials[Index];

		FMaterial Material;
		Material.Name                       = Source.Name;
		Material.BaseColorTexture           = GetOrCreateTexture(Source.BaseColorImage);
		Material.Constants.BaseColorTint    = Source.BaseColorFactor;
		Material.Constants.Shininess        = FMath::Lerp(256.0f, 8.0f, FMath::Clamp(Source.RoughnessFactor, 0.0f, 1.0f));
		Material.Constants.SpecularStrength = FMath::Lerp(0.5f, 0.1f, FMath::Clamp(Source.RoughnessFactor, 0.0f, 1.0f));
		Material.Constants.SpecularColor    = FVector3::Lerp(FVector3(0.04f), FVector3(0.6f), FMath::Clamp(Source.MetallicFactor, 0.0f, 1.0f));
		MaterialHandles[Index]              = Resources.CreateMaterial(Material);
	}

	// 메시
	std::vector<FMeshHandle> MeshHandles(Model.Meshes.size());
	for (size_t Index = 0; Index < Model.Meshes.size(); ++Index)
	{
		if (!Model.Meshes[Index].Data.Vertices.empty() && !Model.Meshes[Index].Data.Indices.empty())
		{
			MeshHandles[Index] = Resources.CreateMesh(Model.Meshes[Index].Data, FStringConv::ToWide(Model.Name + "/" + Model.Meshes[Index].Name));
		}
	}

	// 엔티티 계층
	for (int32 RootNode : Model.RootNodes)
	{
		InstantiateNode(Model, RootNode, Root, Scene, MeshHandles, MaterialHandles, Resources.GetDefaultMaterial());
	}

	E_LOG(LogRenderer, Display, "모델 배치: {} (메시 {}, 머티리얼 {}, 텍스처 {})", Model.Name, MeshHandles.size(),
	      MaterialHandles.size(), ImageTextures.size());
}

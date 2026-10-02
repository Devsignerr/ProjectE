#include "Renderer/ModelLoader.h"

#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "Renderer/AssetCache.h"
#include "Renderer/ResourceManager.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	void InstantiateNode(const FModelData& Model, int32 NodeIndex, FEntity ParentEntity, FScene& Scene,
	                     const std::vector<FMeshHandle>& MeshHandles, const std::vector<FMaterialHandle>& MaterialHandles,
	                     FMaterialHandle DefaultMaterial, std::vector<FEntity>& NodeEntities)
	{
		const FModelNode& Node   = Model.Nodes[NodeIndex];
		const FEntity     Entity = Scene.CreateEntity(Node.Name);
		NodeEntities[NodeIndex]  = Entity;
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
			InstantiateNode(Model, Child, Entity, Scene, MeshHandles, MaterialHandles, DefaultMaterial, NodeEntities);
		}
	}

	// 스킨 노드의 메시 엔티티(노드 자신 또는 primitive 자식)에 조인트 바인딩 부착. 모든 노드 생성 후 호출
	void AttachSkins(const FModelData& Model, FScene& Scene, const std::vector<FEntity>& NodeEntities)
	{
		FRegistry& Registry = Scene.GetRegistry();
		for (size_t NodeIndex = 0; NodeIndex < Model.Nodes.size(); ++NodeIndex)
		{
			const FModelNode& Node = Model.Nodes[NodeIndex];
			if (Node.Skin < 0 || !Registry.IsValid(NodeEntities[NodeIndex]))
			{
				continue;
			}
			const FModelSkin& Skin = Model.Skins[Node.Skin];
			FSkinComponent    Binding;
			Binding.InverseBindMatrices = Skin.InverseBindMatrices;
			Binding.Joints.reserve(Skin.Joints.size());
			for (int32 Joint : Skin.Joints)
			{
				Binding.Joints.push_back(NodeEntities[Joint]);
			}

			std::vector<FEntity> Targets = { NodeEntities[NodeIndex] };
			if (Node.Meshes.size() > 1)
			{
				Targets = Scene.GetChildren(NodeEntities[NodeIndex]);
			}
			for (FEntity Target : Targets)
			{
				if (Registry.Has<FStaticMeshComponent>(Target))
				{
					Registry.Emplace<FSkinComponent>(Target) = Binding;
				}
			}
		}
	}

	// 애니메이션이 있으면 루트에 FAnimationComponent (이미 있으면 설정 유지 — 씬 파일에서 복원된 경우) + 런타임 연결
	void AttachAnimation(const FModelData& Model, FScene& Scene, FEntity Root, std::vector<FEntity> NodeEntities,
	                     const std::shared_ptr<const FModelMetadata>& Metadata)
	{
		if (Model.Animations.empty())
		{
			return;
		}
		std::vector<int32>     Parents(Model.Nodes.size());
		std::vector<FNodePose> RestPose(Model.Nodes.size());
		for (size_t Index = 0; Index < Model.Nodes.size(); ++Index)
		{
			const FModelNode& Node = Model.Nodes[Index];
			Parents[Index]              = Node.Parent;
			RestPose[Index].Translation = Node.Translation;
			RestPose[Index].Rotation    = Node.Rotation;
			RestPose[Index].Scale       = Node.Scale;
		}

		FAnimationComponent& Animation = Scene.GetRegistry().GetOrEmplace<FAnimationComponent>(Root);
		Animation.Runtime              = FAnimationRuntime{};
		Animation.Runtime.Set          = MakeAnimationSet(Model.Animations, std::move(Parents), std::move(RestPose));
		Animation.Runtime.NodeEntities = std::move(NodeEntities);
		Animation.Runtime.Metadata     = Metadata;
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
	const FModelResources* Model = LoadModelResources(Path, Resources);
	if (Model == nullptr)
	{
		return NullEntity;
	}

	const FEntity Root = Scene.CreateEntity(Model->Model.Name);
	Scene.SetParent(Root, Parent);
	Scene.GetRegistry().Emplace<FModelComponent>(Root).AssetPath = MakeAssetPath(Path);
	InstantiateEntities(*Model, Scene, Resources, Root);
	return Root;
}

bool FModelLoader::LoadIntoEntity(const std::filesystem::path& Path, FScene& Scene, FResourceManager& Resources, FEntity Root)
{
	const FModelResources* Model = LoadModelResources(Path, Resources);
	if (Model == nullptr)
	{
		return false;
	}
	InstantiateEntities(*Model, Scene, Resources, Root);
	return true;
}

const FModelResources* FModelLoader::LoadModelResources(const std::filesystem::path& Path, FResourceManager& Resources)
{
	std::error_code             ErrorCode;
	const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	const std::wstring          Key       = (ErrorCode ? Path : Canonical).wstring();
	if (const FModelResources* Cached = Resources.FindModelResources(Key))
	{
		return Cached;
	}

	FModelData Model;
	if (FAssetCache::LoadModelAsset(Path, Model) == FAssetCache::ESource::Failed)
	{
		return nullptr;
	}
	FModelResources Created = CreateResources(std::move(Model), Resources);
	*Created.Metadata        = FModelMetadata::LoadForSource(Path);
	return &Resources.AddModelResources(Key, std::move(Created));
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
	InstantiateEntities(CreateResources(Model, Resources), Scene, Resources, Root);
}

FModelResources FModelLoader::CreateResources(FModelData Model, FResourceManager& Resources)
{
	FModelResources Result;

	// 텍스처: 쿠킹 텍스처는 용도(색공간)가 정해져 있다. 비압축 이미지는 색상 슬롯(베이스/발광)만 sRGB. (이미지, 색공간)별로 한 번만 생성
	std::unordered_map<int64, FTextureHandle> ImageTextures;
	auto GetOrCreateTexture = [&](int32 ImageIndex, bool bSRGB) -> FTextureHandle {
		if (ImageIndex < 0 || ImageIndex >= static_cast<int32>(Model.Images.size()))
		{
			return FTextureHandle{};
		}
		const int64 Key = static_cast<int64>(ImageIndex) * 2 + (bSRGB ? 1 : 0);
		if (const auto Found = ImageTextures.find(Key); Found != ImageTextures.end())
		{
			return Found->second;
		}
		const FModelImage& Image = Model.Images[ImageIndex];
		FTextureHandle     Handle;
		if (Image.Texture.IsValid())
		{
			Handle = Resources.CreateTexture(Image.Texture, FStringConv::ToWide(Model.Name + "/" + Image.Name));
		}
		else if (Image.Image.IsValid())
		{
			Handle = Resources.CreateTexture(Image.Image, bSRGB, FStringConv::ToWide(Model.Name + "/" + Image.Name));
		}
		ImageTextures[Key] = Handle;
		return Handle;
	};

	// 머티리얼 (glTF 금속/거칠기 → 엔진 PBR, 1:1 대응)
	Result.Materials.resize(Model.Materials.size());
	for (size_t Index = 0; Index < Model.Materials.size(); ++Index)
	{
		const FModelMaterial& Source = Model.Materials[Index];

		FMaterial Material;
		Material.Name                                  = Source.Name;
		Material.Constants.BaseColorFactor             = Source.BaseColorFactor;
		Material.Constants.EmissiveFactor              = Source.EmissiveFactor;
		Material.Constants.Metallic                    = Source.MetallicFactor;
		Material.Constants.Roughness                   = Source.RoughnessFactor;
		Material.Constants.NormalScale                 = Source.NormalScale;
		Material.Constants.OcclusionStrength           = Source.OcclusionStrength;
		Material.Constants.AlphaCutoff                 = Source.AlphaCutoff;
		Material.BlendMode                             = Source.BlendMode;
		Material.bTwoSided                             = Source.bTwoSided;
		Material.Textures[MaterialSlot_BaseColor]         = GetOrCreateTexture(Source.BaseColorImage, true);
		Material.Textures[MaterialSlot_MetallicRoughness] = GetOrCreateTexture(Source.MetallicRoughnessImage, false);
		Material.Textures[MaterialSlot_Normal]            = GetOrCreateTexture(Source.NormalImage, false);
		Material.Textures[MaterialSlot_Occlusion]         = GetOrCreateTexture(Source.OcclusionImage, false);
		Material.Textures[MaterialSlot_Emissive]          = GetOrCreateTexture(Source.EmissiveImage, true);
		Result.Materials[Index]                        = Resources.CreateMaterial(Material);
	}

	// 메시
	Result.Meshes.resize(Model.Meshes.size());
	for (size_t Index = 0; Index < Model.Meshes.size(); ++Index)
	{
		FModelMesh& Mesh = Model.Meshes[Index];
		if (!Mesh.Data.Vertices.empty() && !Mesh.Data.Indices.empty())
		{
			const std::wstring DebugName = FStringConv::ToWide(Model.Name + "/" + Mesh.Name);
			Result.Meshes[Index] = Mesh.SkinVertices.empty() ? Resources.CreateMesh(Mesh.Data, DebugName)
			                                                 : Resources.CreateSkinnedMesh(Mesh.Data, Mesh.SkinVertices, DebugName);
		}
		// GPU에 올린 뒤 CPU 정점 데이터는 필요 없다 (엔티티 배치는 머티리얼 번호만 사용)
		Mesh.Data         = FMeshData{};
		Mesh.SkinVertices = {};
	}
	Model.Images.clear();

	Result.TextureCount = ImageTextures.size();
	Result.Metadata     = std::make_shared<FModelMetadata>();
	Result.Model        = std::move(Model);
	E_LOG(LogRenderer, Display, "모델 리소스 생성: {} (메시 {}, 머티리얼 {}, 텍스처 {})", Result.Model.Name, Result.Meshes.size(),
	      Result.Materials.size(), Result.TextureCount);
	return Result;
}

void FModelLoader::InstantiateEntities(const FModelResources& Resources, FScene& Scene, FResourceManager& ResourceManager, FEntity Root)
{
	const FModelData&    Model = Resources.Model;
	std::vector<FEntity> NodeEntities(Model.Nodes.size(), NullEntity);
	for (int32 RootNode : Model.RootNodes)
	{
		InstantiateNode(Model, RootNode, Root, Scene, Resources.Meshes, Resources.Materials, ResourceManager.GetDefaultMaterial(), NodeEntities);
	}
	AttachSkins(Model, Scene, NodeEntities);
	// 소켓 해석용 노드 연결 (모델 컴포넌트가 있는 루트만 — 절차 생성/테스트 배치는 없을 수 있다)
	if (FModelComponent* ModelComponent = Scene.GetRegistry().TryGet<FModelComponent>(Root))
	{
		ModelComponent->Runtime.Metadata     = Resources.Metadata;
		ModelComponent->Runtime.NodeEntities = NodeEntities;
	}
	AttachAnimation(Model, Scene, Root, std::move(NodeEntities), Resources.Metadata);
}

bool FModelLoader::IsModelFile(const std::filesystem::path& Path)
{
	std::wstring Extension = Path.extension().wstring();
	std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
	return Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx";
}

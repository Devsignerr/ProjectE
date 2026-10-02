#include "Core/Testing/TestFramework.h"
#include "Renderer/ResourceCollector.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <string>
#include <vector>

// 리소스 수거 (Phase 37) 순수 도달성 규칙 — Renderer/ResourceCollector.h 머리 주석

namespace
{
	FTextureHandle  Tex(uint32 Index) { return FTextureHandle{ Index, 0 }; }
	FMaterialHandle Mat(uint32 Index) { return FMaterialHandle{ Index, 0 }; }
	FMeshHandle     Mesh(uint32 Index) { return FMeshHandle{ Index, 0 }; }

	template <typename T>
	bool Contains(const std::vector<T>& Values, const T& Value)
	{
		return std::find(Values.begin(), Values.end(), Value) != Values.end();
	}

	ResourceGc::FMaterialNode MakeMaterial(FMaterialHandle Handle, std::vector<FTextureHandle> Textures, bool bCollectible)
	{
		ResourceGc::FMaterialNode Node;
		Node.Handle       = Handle;
		Node.Textures     = std::move(Textures);
		Node.bCollectible = bCollectible;
		return Node;
	}

	// 맵 하나 = 경로 머티리얼 하나 + 그 텍스처 하나 (맵마다 다른 번호)
	struct FFakeManager
	{
		ResourceGc::FGraph Graph;
		uint32             NextIndex = 100;

		FMaterialHandle LoadMap()
		{
			const FTextureHandle  Texture  = Tex(NextIndex++);
			const FMaterialHandle Material = Mat(NextIndex++);
			Graph.CollectibleTextures.push_back(Texture);
			Graph.Materials.push_back(MakeMaterial(Material, { Texture }, true));
			return Material;
		}

		void Apply(const ResourceGc::FGarbage& Garbage)
		{
			std::erase_if(Graph.Materials, [&](const ResourceGc::FMaterialNode& Node) { return Contains(Garbage.Materials, Node.Handle); });
			std::erase_if(Graph.CollectibleTextures, [&](FTextureHandle Handle) { return Contains(Garbage.Textures, Handle); });
		}
	};
} // namespace

E_TEST(ResourceCollector_UnreferencedPathResourcesAreGarbage)
{
	ResourceGc::FGraph Graph;
	Graph.CollectibleTextures = { Tex(1), Tex(2), Tex(3) };
	Graph.Materials.push_back(MakeMaterial(Mat(1), { Tex(1) }, true));  // 씬이 쓰는 경로 머티리얼
	Graph.Materials.push_back(MakeMaterial(Mat(2), { Tex(2) }, true));  // 아무도 안 씀
	Graph.Materials.push_back(MakeMaterial(Mat(3), { Tex(3) }, false)); // 앱이 직접 만든 머티리얼 (항상 루트)

	FResourceRoots Roots;
	Roots.Add(Mat(1));
	Roots.Add(FMaterialHandle{}); // 무효 핸들은 무시

	const ResourceGc::FGarbage Garbage = ResourceGc::Compute(Graph, Roots);
	E_EXPECT_EQ(Garbage.Materials.size(), size_t(1));
	E_EXPECT_TRUE(Contains(Garbage.Materials, Mat(2)));
	E_EXPECT_EQ(Garbage.Textures.size(), size_t(1));
	E_EXPECT_TRUE(Contains(Garbage.Textures, Tex(2)));
	E_EXPECT_EQ(Roots.Materials.size(), size_t(1));
}

E_TEST(ResourceCollector_SharedTextureSurvives)
{
	// 두 머티리얼이 같은 텍스처를 쓰고 하나만 버려지면 텍스처는 남는다. 루트 텍스처(UI 등)도 남는다
	ResourceGc::FGraph Graph;
	Graph.CollectibleTextures = { Tex(1), Tex(2) };
	Graph.Materials.push_back(MakeMaterial(Mat(1), { Tex(1) }, true));
	Graph.Materials.push_back(MakeMaterial(Mat(2), { Tex(1), Tex(1) }, true));

	FResourceRoots Roots;
	Roots.Add(Mat(1));
	Roots.Add(Tex(2));
	const ResourceGc::FGarbage Garbage = ResourceGc::Compute(Graph, Roots);
	E_EXPECT_TRUE(Contains(Garbage.Materials, Mat(2)));
	E_EXPECT_TRUE(Garbage.Textures.empty());
}

E_TEST(ResourceCollector_ModelReachableThroughAnyMeshOrMaterial)
{
	ResourceGc::FGraph Graph;
	ResourceGc::FModelNode Used;
	Used.Key       = L"used.glb";
	Used.Meshes    = { Mesh(1), Mesh(2) };
	Used.Materials = { Mat(1) };
	ResourceGc::FModelNode Unused;
	Unused.Key       = L"unused.glb";
	Unused.Meshes    = { Mesh(3) };
	Unused.Materials = { Mat(2) };
	Graph.Models               = { Used, Unused };
	Graph.Materials            = { MakeMaterial(Mat(1), { Tex(1) }, true), MakeMaterial(Mat(2), { Tex(2) }, true) };
	Graph.CollectibleTextures = { Tex(1), Tex(2) }; // 모델이 만든 텍스처

	FResourceRoots Roots;
	Roots.Add(Mesh(2)); // 인스턴스 노드 하나만 남아 있어도 모델 전체가 산다

	const ResourceGc::FGarbage Garbage = ResourceGc::Compute(Graph, Roots);
	E_EXPECT_EQ(Garbage.Models.size(), size_t(1));
	E_EXPECT_TRUE(Contains(Garbage.Models, std::wstring(L"unused.glb")));
	E_EXPECT_TRUE(Contains(Garbage.Meshes, Mesh(3)) && !Contains(Garbage.Meshes, Mesh(1)));
	E_EXPECT_TRUE(Contains(Garbage.Materials, Mat(2)) && !Contains(Garbage.Materials, Mat(1)));
	E_EXPECT_TRUE(Contains(Garbage.Textures, Tex(2)) && !Contains(Garbage.Textures, Tex(1)));

	// 머티리얼만 루트여도 (편집기가 든 모델 머티리얼)
	FResourceRoots MaterialRoot;
	MaterialRoot.Add(Mat(2));
	const ResourceGc::FGarbage ByMaterial = ResourceGc::Compute(Graph, MaterialRoot);
	E_EXPECT_TRUE(Contains(ByMaterial.Models, std::wstring(L"used.glb")) && !Contains(ByMaterial.Models, std::wstring(L"unused.glb")));
}

E_TEST(ResourceCollector_ParticleReferenceKeepsTextures)
{
	ResourceGc::FGraph Graph;
	ResourceGc::FParticleNode Live;
	Live.Key         = L"fire.eparticle";
	Live.bReferenced = true;
	Live.Textures    = { Tex(1) };
	ResourceGc::FParticleNode Dead;
	Dead.Key      = L"smoke.eparticle";
	Dead.Textures = { Tex(2) };
	Graph.Particles           = { Live, Dead };
	Graph.CollectibleTextures = { Tex(1), Tex(2) };

	const ResourceGc::FGarbage Garbage = ResourceGc::Compute(Graph, FResourceRoots{});
	E_EXPECT_EQ(Garbage.Particles.size(), size_t(1));
	E_EXPECT_TRUE(Contains(Garbage.Particles, std::wstring(L"smoke.eparticle")));
	E_EXPECT_TRUE(Contains(Garbage.Textures, Tex(2)) && !Contains(Garbage.Textures, Tex(1)));
}

E_TEST(ResourceCollector_AddSceneCollectsComponentHandles)
{
	FScene                Scene;
	const FEntity         A    = Scene.CreateEntity("A");
	FStaticMeshComponent& Mesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(A);
	Mesh.Mesh                  = FMeshHandle{ 7, 1 };
	Mesh.Material              = FMaterialHandle{ 3, 2 };
	const FEntity B            = Scene.CreateEntity("B");
	Scene.GetRegistry().Emplace<FDecalComponent>(B).Material = FMaterialHandle{ 4, 0 };
	Scene.GetRegistry().Emplace<FStaticMeshComponent>(Scene.CreateEntity("Empty")); // 무효 핸들

	FResourceRoots Roots;
	Roots.AddScene(Scene);
	E_EXPECT_EQ(Roots.Meshes.size(), size_t(1));
	E_EXPECT_EQ(Roots.Materials.size(), size_t(2));
	E_EXPECT_TRUE(Roots.Meshes.contains(FMeshHandle{ 7, 1 }));
	E_EXPECT_TRUE(Roots.Materials.contains(FMaterialHandle{ 4, 0 }));
}

E_TEST(ResourceCollector_TravelLoopStaysFlat)
{
	// 맵 A ↔ B 반복: 수거 뒤 살아 있는 리소스 수가 매번 같아야 한다 (공용 머티리얼 1 + 현재 맵 1)
	FFakeManager Manager;
	const FTextureHandle  SharedTexture = Tex(1);
	const FMaterialHandle Shared        = Mat(1);
	Manager.Graph.CollectibleTextures.push_back(SharedTexture);
	Manager.Graph.Materials.push_back(MakeMaterial(Shared, { SharedTexture }, true));

	std::vector<size_t> Counts;
	for (int32 Trip = 0; Trip < 10; ++Trip)
	{
		const FMaterialHandle MapMaterial = Manager.LoadMap(); // 맵을 바꿀 때마다 새로 로드 (이전 맵 것은 루트에서 빠짐)
		FResourceRoots        Roots;
		Roots.Add(Shared);
		Roots.Add(MapMaterial);
		Manager.Apply(ResourceGc::Compute(Manager.Graph, Roots));
		Counts.push_back(Manager.Graph.Materials.size() + Manager.Graph.CollectibleTextures.size());
	}
	E_EXPECT_TRUE(std::all_of(Counts.begin(), Counts.end(), [](size_t Count) { return Count == 4; }));
	E_EXPECT_TRUE(Contains(Manager.Graph.CollectibleTextures, SharedTexture));
}

E_TEST(ResourceCollector_FormatBytes)
{
	E_EXPECT_EQ(ResourceGc::FormatBytes(512), std::string("0.5 KB"));
	E_EXPECT_EQ(ResourceGc::FormatBytes(3ull * 1024 * 1024), std::string("3.0 MB"));
	E_EXPECT_EQ(ResourceGc::FormatBytes(2ull * 1024 * 1024 * 1024), std::string("2.00 GB"));
	FResourceMemoryStats Stats;
	Stats.bHasVideoMemory = true;
	Stats.LocalUsage      = 200;
	Stats.LocalBudget     = 100;
	E_EXPECT_TRUE(Stats.IsOverBudget());
	E_EXPECT_EQ(ResourceGc::FormatMemoryStats(Stats).size(), size_t(4));
}

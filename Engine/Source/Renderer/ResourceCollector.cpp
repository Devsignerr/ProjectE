#include "Renderer/ResourceCollector.h"

#include "Core/Console/Console.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Components.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <format>
#include <iterator>

E_DECLARE_LOG_CATEGORY(LogRenderer)

// ---------------------------------------------------------------- 루트

void FResourceRoots::Add(FMeshHandle Handle)
{
	if (Handle.IsValid())
	{
		Meshes.insert(Handle);
	}
}

void FResourceRoots::Add(FMaterialHandle Handle)
{
	if (Handle.IsValid())
	{
		Materials.insert(Handle);
	}
}

void FResourceRoots::Add(FTextureHandle Handle)
{
	if (Handle.IsValid())
	{
		Textures.insert(Handle);
	}
}

void FResourceRoots::AddScene(FScene& Scene)
{
	FRegistry& Registry = Scene.GetRegistry();
	Registry.View<FStaticMeshComponent>().Each([this](FEntity, FStaticMeshComponent& Mesh) {
		Add(Mesh.Mesh);
		Add(Mesh.Material);
	});
	Registry.View<FDecalComponent>().Each([this](FEntity, FDecalComponent& Decal) { Add(Decal.Material); });
}

// ---------------------------------------------------------------- 순수 도달성 계산

ResourceGc::FGarbage ResourceGc::Compute(const FGraph& Graph, const FResourceRoots& Roots)
{
	FGarbage Garbage;

	// 1. 머티리얼 표시: 루트 + 직접 만든 머티리얼(만든 쪽이 소유 — 항상 살아 있음)
	std::unordered_set<FMaterialHandle> MarkedMaterials = Roots.Materials;
	for (const FMaterialNode& Node : Graph.Materials)
	{
		if (!Node.bCollectible)
		{
			MarkedMaterials.insert(Node.Handle);
		}
	}

	// 2. 모델: 메시나 머티리얼 하나라도 루트면 모델 전체가 산다 (인스턴스 노드가 메시/머티리얼을 나눠 든다)
	for (const FModelNode& Model : Graph.Models)
	{
		const bool bReachable =
			std::any_of(Model.Meshes.begin(), Model.Meshes.end(), [&](FMeshHandle Handle) { return Roots.Meshes.contains(Handle); }) ||
			std::any_of(Model.Materials.begin(), Model.Materials.end(), [&](FMaterialHandle Handle) { return Roots.Materials.contains(Handle); });
		if (bReachable)
		{
			MarkedMaterials.insert(Model.Materials.begin(), Model.Materials.end());
			continue;
		}
		Garbage.Models.push_back(Model.Key);
		for (const FMeshHandle Mesh : Model.Meshes)
		{
			if (Mesh.IsValid() && std::find(Garbage.Meshes.begin(), Garbage.Meshes.end(), Mesh) == Garbage.Meshes.end())
			{
				Garbage.Meshes.push_back(Mesh);
			}
		}
	}

	// 3. 텍스처 표시: 루트 + 표시된 머티리얼 + 참조 중인 파티클 에셋
	std::unordered_set<FTextureHandle> MarkedTextures = Roots.Textures;
	for (const FParticleNode& Particle : Graph.Particles)
	{
		if (Particle.bReferenced)
		{
			MarkedTextures.insert(Particle.Textures.begin(), Particle.Textures.end());
		}
		else
		{
			Garbage.Particles.push_back(Particle.Key);
		}
	}
	for (const FMaterialNode& Node : Graph.Materials)
	{
		if (MarkedMaterials.contains(Node.Handle))
		{
			MarkedTextures.insert(Node.Textures.begin(), Node.Textures.end());
		}
		else if (Node.bCollectible)
		{
			Garbage.Materials.push_back(Node.Handle);
		}
	}
	for (const FTextureHandle Texture : Graph.CollectibleTextures)
	{
		if (Texture.IsValid() && !MarkedTextures.contains(Texture) &&
		    std::find(Garbage.Textures.begin(), Garbage.Textures.end(), Texture) == Garbage.Textures.end())
		{
			Garbage.Textures.push_back(Texture);
		}
	}
	return Garbage;
}

std::string ResourceGc::FormatBytes(uint64 Bytes)
{
	constexpr double Kb = 1024.0;
	constexpr double Mb = Kb * 1024.0;
	constexpr double Gb = Mb * 1024.0;
	const double     Value = static_cast<double>(Bytes);
	if (Value >= Gb)
	{
		return std::format("{:.2f} GB", Value / Gb);
	}
	if (Value >= Mb)
	{
		return std::format("{:.1f} MB", Value / Mb);
	}
	return std::format("{:.1f} KB", Value / Kb);
}

std::vector<std::string> ResourceGc::FormatMemoryStats(const FResourceMemoryStats& Stats)
{
	std::vector<std::string> Lines;
	if (Stats.bHasVideoMemory)
	{
		Lines.push_back(std::format("VRAM {} / 예산 {}{}  (공유 {})", FormatBytes(Stats.LocalUsage), FormatBytes(Stats.LocalBudget),
		                            Stats.IsOverBudget() ? " 초과!" : "", FormatBytes(Stats.NonLocalUsage)));
	}
	Lines.push_back(std::format("텍스처 {} (경로 {}) {}", Stats.Textures, Stats.PathTextures, FormatBytes(Stats.TextureBytes)));
	Lines.push_back(std::format("메시 {} {}", Stats.Meshes, FormatBytes(Stats.MeshBytes)));
	Lines.push_back(std::format("머티리얼 {} (경로 {})  모델 {}  파티클 {}", Stats.Materials, Stats.PathMaterials, Stats.Models, Stats.ParticleSystems));
	return Lines;
}

// ---------------------------------------------------------------- FResourceManager (수거 / 통계)

void FResourceManager::InitCollector()
{
	FConsoleManager& Console = FConsoleManager::Get();
	if (Console.FindCommand("r.CollectResources") != nullptr)
	{
		return; // 다른 리소스 관리자가 이미 가졌다 (한 프로세스에 보통 하나)
	}
	Console.RegisterCommand({ "r.CollectResources", "쓰지 않는 경로 리소스(메시/텍스처/머티리얼/모델/파티클)를 다음 프레임에 수거",
	                          [this](const std::vector<std::string>&, const FConsoleOutput& Output) {
		                          RequestGarbageCollection("콘솔", 1, true); // 한 프레임 그린 뒤 (렌더러 캐시 루트가 채워지게)
		                          Output.Print("다음 프레임에 리소스를 수거합니다");
	                          } });
	Console.RegisterCommand({ "r.ResourceStats", "리소스 수/바이트와 VRAM 사용량·예산 출력",
	                          [this](const std::vector<std::string>&, const FConsoleOutput& Output) {
		                          for (const std::string& Line : ResourceGc::FormatMemoryStats(GetMemoryStats()))
		                          {
			                          Output.Print(Line);
		                          }
	                          } });
	bOwnsConsoleCommands = true;
}

void FResourceManager::ShutdownCollector()
{
	if (bOwnsConsoleCommands)
	{
		FConsoleManager::Get().UnregisterCommand("r.CollectResources");
		FConsoleManager::Get().UnregisterCommand("r.ResourceStats");
		bOwnsConsoleCommands = false;
	}
	RootProviders.clear();
	PendingCollectTicks = -1;
	PendingCollectReason.clear();
}

uint32 FResourceManager::AddRootProvider(FResourceRootProvider Provider)
{
	const uint32 Id = NextRootProviderId++;
	RootProviders.push_back({ Id, std::move(Provider) });
	return Id;
}

void FResourceManager::RemoveRootProvider(uint32 Id)
{
	std::erase_if(RootProviders, [Id](const FRootProviderEntry& Entry) { return Entry.Id == Id; });
}

void FResourceManager::RequestGarbageCollection(std::string_view Reason, uint32 DelayFrames, bool bForce)
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 자동 요청은 r.ResourceAutoCollect로 끌 수 있다 (콘솔 명령/통계 창은 항상)
	if (!bForce && !RendererCVars::ResourceAutoCollect.Get())
	{
		return;
	}
	if (PendingCollectTicks < 0)
	{
		PendingCollectReason = Reason;
	}
	else if (PendingCollectReason.find(Reason) == std::string::npos)
	{
		PendingCollectReason += std::format(", {}", Reason);
	}
	PendingCollectTicks = std::max(PendingCollectTicks, static_cast<int32>(DelayFrames));
}

void FResourceManager::Tick()
{
	if (Rhi == nullptr)
	{
		return;
	}
	++TickCount;
	if (PendingCollectTicks >= 0 && PendingCollectTicks-- == 0)
	{
		const std::string Reason = std::move(PendingCollectReason);
		PendingCollectReason.clear();
		PendingCollectTicks = -1;
		CollectGarbage(Reason);
	}

	// VRAM 예산 초과 경고 (약 2초마다 확인, 넘어간 순간 한 번)
	if (TickCount % 120 == 0)
	{
		FD3D12Device::FVideoMemoryInfo Info;
		if (Rhi->GetDevice().QueryVideoMemory(Info))
		{
			const bool bOver = Info.LocalBudget > 0 && Info.LocalUsage > Info.LocalBudget;
			if (bOver && !bWarnedOverBudget)
			{
				E_LOG(LogRenderer, Warning, "VRAM 예산 초과: 사용 {} / 예산 {} — 텍스처/메시가 시스템 메모리로 밀려 느려질 수 있습니다",
				      ResourceGc::FormatBytes(Info.LocalUsage), ResourceGc::FormatBytes(Info.LocalBudget));
			}
			bWarnedOverBudget = bOver;
		}
	}
}

FResourceCollectResult FResourceManager::CollectGarbage(std::string_view Reason)
{
	FResourceCollectResult Result;
	if (Rhi == nullptr)
	{
		return Result;
	}

	// 루트
	FResourceRoots Roots;
	for (const FRootProviderEntry& Entry : RootProviders)
	{
		Entry.Provider(Roots);
	}

	// 그래프: 수거 대상 = 경로 캐시 텍스처/머티리얼 + 모델 + 파티클 에셋
	ResourceGc::FGraph                  Graph;
	std::unordered_set<FMaterialHandle> CollectibleMaterials;
	std::unordered_set<FTextureHandle>  CollectibleTextures;
	for (const auto& [Key, Handle] : MaterialCache)
	{
		if (Materials.IsValid(Handle))
		{
			CollectibleMaterials.insert(Handle);
		}
	}
	for (const auto& [Key, Handle] : TextureCache)
	{
		if (Textures.IsValid(Handle))
		{
			CollectibleTextures.insert(Handle);
		}
	}
	for (const auto& [Key, Model] : ModelCache)
	{
		if (!Model)
		{
			continue;
		}
		ResourceGc::FModelNode& Node = Graph.Models.emplace_back();
		Node.Key                     = Key;
		Node.Meshes                  = Model->Meshes;
		Node.Materials               = Model->Materials;
		for (const FMaterialHandle Handle : Model->Materials)
		{
			const FMaterial* Material = Materials.Get(Handle);
			if (Material == nullptr || Handle == DefaultMaterial)
			{
				continue;
			}
			CollectibleMaterials.insert(Handle);
			for (const FTextureHandle Texture : Material->Textures)
			{
				// 모델이 만든 텍스처 (기본 텍스처 제외)
				if (Textures.IsValid(Texture) && Texture != WhiteTexture && Texture != FlatNormalTexture)
				{
					CollectibleTextures.insert(Texture);
				}
			}
		}
	}
	for (const auto& [Key, System] : ParticleCache)
	{
		ResourceGc::FParticleNode& Node = Graph.Particles.emplace_back();
		Node.Key                        = Key;
		Node.bReferenced                = System && System.use_count() > 1; // 캐시 밖(컴포넌트/편집기)에서 들고 있다
		if (System)
		{
			for (const FParticleEmitter& Emitter : System->Emitters)
			{
				for (const FParticleRendererSettings& Renderer : Emitter.Renderers)
				{
					if (Renderer.Texture.IsValid())
					{
						Node.Textures.push_back(Renderer.Texture);
					}
				}
			}
		}
	}
	Materials.ForEach([&](FMaterialHandle Handle, FMaterial& Material) {
		ResourceGc::FMaterialNode& Node = Graph.Materials.emplace_back();
		Node.Handle                     = Handle;
		Node.Textures.assign(std::begin(Material.Textures), std::end(Material.Textures));
		Node.bCollectible = Handle != DefaultMaterial && CollectibleMaterials.contains(Handle);
	});
	Graph.CollectibleTextures.assign(CollectibleTextures.begin(), CollectibleTextures.end());

	const ResourceGc::FGarbage Garbage = ResourceGc::Compute(Graph, Roots);

	// 해제 (모두 지연 — 진행 중인 프레임이 읽을 수 있다). 머티리얼 → 메시 → 텍스처 순 (텍스처 해제가 남은 머티리얼 테이블을 다시 만들지 않게)
	std::unordered_set<std::wstring> FreedMaterialKeys;
	for (const auto& [Key, Handle] : MaterialCache)
	{
		if (std::find(Garbage.Materials.begin(), Garbage.Materials.end(), Handle) != Garbage.Materials.end())
		{
			FreedMaterialKeys.insert(FMaterialAsset::MakePathKey(Key));
		}
	}
	for (const FMaterialHandle Handle : Garbage.Materials)
	{
		if (Materials.IsValid(Handle))
		{
			DestroyMaterial(Handle);
			++Result.Materials;
		}
	}
	for (const FMeshHandle Handle : Garbage.Meshes)
	{
		if (Meshes.IsValid(Handle))
		{
			DestroyMesh(Handle);
			++Result.Meshes;
		}
	}
	for (const FTextureHandle Handle : Garbage.Textures)
	{
		if (Textures.IsValid(Handle))
		{
			DestroyTexture(Handle);
			++Result.Textures;
		}
	}
	for (const std::wstring& Key : Garbage.Models)
	{
		Result.Models += static_cast<uint32>(ModelCache.erase(Key));
	}
	for (const std::wstring& Key : Garbage.Particles)
	{
		Result.Particles += static_cast<uint32>(ParticleCache.erase(Key));
	}

	// 경로 캐시 정리: 이미 지워진 핸들 항목 + 수거한 머티리얼의 편집 중 원본 (Phase 36 EditedMaterialSources)
	std::erase_if(TextureCache, [this](const auto& Entry) { return !Textures.IsValid(Entry.second); });
	std::erase_if(MaterialCache, [this](const auto& Entry) { return !Materials.IsValid(Entry.second); });
	std::erase_if(EditedMaterialSources, [&](const auto& Entry) { return FreedMaterialKeys.contains(Entry.first); });

	const FResourceMemoryStats Stats = GetMemoryStats();
	E_LOG(LogRenderer, Display,
	      "[리소스 수거] {}: 해제 메시 {}, 머티리얼 {}, 텍스처 {}, 모델 {}, 파티클 {} → 남음 메시 {} ({}), 머티리얼 {}, 텍스처 {} ({}), 모델 {}, 파티클 {}{}",
	      Reason, Result.Meshes, Result.Materials, Result.Textures, Result.Models, Result.Particles, Stats.Meshes, ResourceGc::FormatBytes(Stats.MeshBytes),
	      Stats.Materials, Stats.Textures, ResourceGc::FormatBytes(Stats.TextureBytes), Stats.Models, Stats.ParticleSystems,
	      Stats.bHasVideoMemory ? std::format(", VRAM {} / {}", ResourceGc::FormatBytes(Stats.LocalUsage), ResourceGc::FormatBytes(Stats.LocalBudget))
	                            : std::string());
	return Result;
}

FResourceMemoryStats FResourceManager::GetMemoryStats()
{
	FResourceMemoryStats Stats;
	if (Rhi == nullptr)
	{
		return Stats;
	}
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	Textures.ForEach([&](FTextureHandle, FD3D12Texture& Texture) {
		++Stats.Textures;
		if (ID3D12Resource* Resource = Texture.GetResource())
		{
			const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
			Stats.TextureBytes += Device->GetResourceAllocationInfo(0, 1, &Desc).SizeInBytes;
		}
	});
	Meshes.ForEach([&](FMeshHandle, FStaticMesh& Mesh) {
		++Stats.Meshes;
		Stats.MeshBytes += Mesh.GetGpuBytes();
	});
	Stats.Materials       = static_cast<uint32>(Materials.GetCount());
	Stats.Models          = static_cast<uint32>(ModelCache.size());
	Stats.ParticleSystems = static_cast<uint32>(ParticleCache.size());
	Stats.PathTextures    = static_cast<uint32>(std::count_if(TextureCache.begin(), TextureCache.end(), [this](const auto& Entry) { return Textures.IsValid(Entry.second); }));
	Stats.PathMaterials   = static_cast<uint32>(std::count_if(MaterialCache.begin(), MaterialCache.end(), [this](const auto& Entry) { return Materials.IsValid(Entry.second); }));

	FD3D12Device::FVideoMemoryInfo Info;
	if (Rhi->GetDevice().QueryVideoMemory(Info))
	{
		Stats.bHasVideoMemory = true;
		Stats.LocalUsage      = Info.LocalUsage;
		Stats.LocalBudget     = Info.LocalBudget;
		Stats.NonLocalUsage   = Info.NonLocalUsage;
		Stats.NonLocalBudget  = Info.NonLocalBudget;
	}
	return Stats;
}

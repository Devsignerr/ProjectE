#include "Renderer/FoliageRenderer.h"

#include "Core/CommandLine.h"
#include "Core/StringConv.h"
#include "Renderer/Camera.h"
#include "Renderer/FoliageMeshes.h"
#include "Renderer/LodMath.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/ResourceCollector.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Foliage.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
	constexpr float  CellSize                  = 2000.0f; // cm
	constexpr uint64 UnusedFramesBeforeRelease = 120;

	float DistanceToBox(const FBox& Box, const FVector3& Point)
	{
		const float DX = std::max({ Box.Min.X - Point.X, 0.0f, Point.X - Box.Max.X });
		const float DY = std::max({ Box.Min.Y - Point.Y, 0.0f, Point.Y - Box.Max.Y });
		const float DZ = std::max({ Box.Min.Z - Point.Z, 0.0f, Point.Z - Box.Max.Z });
		return std::sqrt(DX * DX + DY * DY + DZ * DZ);
	}
} // namespace

void FFoliageRenderer::Shutdown()
{
	if (Resources != nullptr)
	{
		for (const auto& [Name, Handle] : Meshes)
		{
			if (Handle.IsValid() && Name.starts_with("foliage:"))
			{
				Resources->DestroyMesh(Handle); // 내장 절차 메시는 이 렌더러가 만들었다 (지연 해제)
			}
		}
	}
	Meshes.clear();
	Materials.clear();
	Caches.clear();
	Resources = nullptr;
}

void FFoliageRenderer::CollectResourceRoots(FResourceRoots& Roots) const
{
	for (const auto& [Asset, Cache] : Caches)
	{
		if (Cache.LastUsedFrame != FrameCounter)
		{
			continue; // 지난 맵/지운 폴리지 (오래되면 Gather가 비운다)
		}
		for (const FTypeCache& Type : Cache.Types)
		{
			Roots.Add(Type.Mesh);
			Roots.Add(Type.Material);
		}
	}
}

FMeshHandle FFoliageRenderer::ResolveMesh(const std::string& Name)
{
	if (const auto Found = Meshes.find(Name); Found != Meshes.end())
	{
		return Found->second;
	}
	FMeshHandle Handle;
	if (Name.starts_with("foliage:"))
	{
		FMeshData Mesh;
		if (FoliageMeshes::Build(std::string_view(Name).substr(8), Mesh))
		{
			Handle = Resources->CreateMesh(Mesh, FStringConv::ToWide("Foliage_" + Name.substr(8)));
		}
	}
	else if (Name.starts_with("primitive:"))
	{
		Handle = Resources->GetOrCreatePrimitiveMesh(std::string_view(Name).substr(10));
	}
	Meshes[Name] = Handle; // 실패도 기억 (매 프레임 다시 만들지 않게)
	return Handle;
}

FMaterialHandle FFoliageRenderer::ResolveMaterial(const std::string& Asset)
{
	if (Asset.empty())
	{
		return FMaterialHandle{};
	}
	if (const auto Found = Materials.find(Asset); Found != Materials.end() && Resources->GetMaterial(Found->second) != nullptr)
	{
		return Found->second;
	}
	const FMaterialHandle Handle = Resources->LoadMaterial(FFoliageLibrary::Get().ResolveAssetPath(Asset));
	Materials[Asset]             = Handle;
	return Handle;
}

void FFoliageRenderer::Rebuild(const FFoliageAsset& Asset, FAssetCache& Cache)
{
	if (Cache.Types.size() != Asset.Types.size())
	{
		Cache.Types.clear();
		Cache.Types.resize(Asset.Types.size());
	}
	for (size_t TypeIndex = 0; TypeIndex < Asset.Types.size(); ++TypeIndex)
	{
		FTypeCache&  Entry   = Cache.Types[TypeIndex];
		const uint64 Counter = TypeIndex < Asset.TypeCounters.size() ? Asset.TypeCounters[TypeIndex] : Asset.ChangeCounter;
		if (Entry.Counter != 0 && Entry.Counter == Counter)
		{
			continue; // 이 타입은 그대로 (칠하는 중 다른 타입을 다시 만들지 않게)
		}
		Entry                     = FTypeCache{};
		Entry.Counter             = Counter;
		const FFoliageType& Type  = Asset.Types[TypeIndex];
		Entry.Mesh                = ResolveMesh(Type.Mesh);
		Entry.Material            = ResolveMaterial(Type.Material);
		const FStaticMesh* Mesh   = Resources->GetMesh(Entry.Mesh);
		if (Mesh == nullptr || TypeIndex >= Asset.Instances.size())
		{
			continue;
		}
		const std::vector<FFoliageInstance>& Instances = Asset.Instances[TypeIndex];
		Entry.Worlds.resize(Instances.size());
		Entry.Bounds.resize(Instances.size());
		std::unordered_map<uint64, uint32> CellIndex;
		for (uint32 Index = 0; Index < static_cast<uint32>(Instances.size()); ++Index)
		{
			Entry.Worlds[Index] = FoliageMath::MakeWorldMatrix(Instances[Index], Type, 1.0f);
			Entry.Bounds[Index] = Mesh->GetLocalBounds().TransformBy(Entry.Worlds[Index]);
			const int32  CX     = static_cast<int32>(std::floor(Instances[Index].Position.X / CellSize));
			const int32  CY     = static_cast<int32>(std::floor(Instances[Index].Position.Y / CellSize));
			const uint64 Key    = (static_cast<uint64>(static_cast<uint32>(CX)) << 32) | static_cast<uint32>(CY);
			auto [It, bNew]     = CellIndex.try_emplace(Key, static_cast<uint32>(Entry.Cells.size()));
			if (bNew)
			{
				Entry.Cells.emplace_back();
			}
			FCell& Cell = Entry.Cells[It->second];
			Cell.Bounds.AddBox(Entry.Bounds[Index]);
			Cell.Instances.push_back(Index);
		}
	}
	Cache.ChangeCounter = Asset.ChangeCounter;
}

void FFoliageRenderer::Gather(FScene& Scene, const FCamera& Camera, const FFrustum& Frustum, const std::function<bool(const FBox&)>& ShadowCaster,
                              FMeshInstanceList& OutInstances)
{
	++FrameCounter;
	TotalInstances = DrawnInstances = ShadowOnlyInstances = VisitedCells = 0;
	if (!bEnabled || Resources == nullptr)
	{
		return;
	}
	std::vector<FFoliageInstanceSet> Sets;
	GatherFoliage(Scene, Sets);
	if (Sets.empty())
	{
		return;
	}
	static const float CommandLineScale = [] {
		const std::wstring Value = FCommandLine::FromProcess().GetValue(L"--foliage-distance");
		return Value.empty() ? 1.0f : std::stof(Value);
	}();
	const float    DistanceScale  = CullDistanceScale * CommandLineScale;
	const FVector3 CameraPosition = Camera.GetPosition();
	const float    TanHalfFov     = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	const bool     bOrthographic  = Camera.IsOrthographic();

	for (const FFoliageInstanceSet& Set : Sets)
	{
		FAssetCache& Cache = Caches[Set.Asset];
		Cache.LastUsedFrame = FrameCounter;
		if (Cache.ChangeCounter != Set.Asset->ChangeCounter || Cache.Types.size() != Set.Asset->Types.size())
		{
			Rebuild(*Set.Asset, Cache);
		}
		if (!Set.Component->bVisible)
		{
			continue;
		}
		for (size_t TypeIndex = 0; TypeIndex < Cache.Types.size(); ++TypeIndex)
		{
			const FFoliageType& Type  = Set.Asset->Types[TypeIndex];
			const FTypeCache&   Entry = Cache.Types[TypeIndex];
			const FStaticMesh*  Mesh  = Resources->GetMesh(Entry.Mesh);
			if (Mesh == nullptr || !Mesh->IsReady()) // 업로드 중이면 이번 프레임은 건너뜀
			{
				continue;
			}
			TotalInstances += static_cast<uint32>(Entry.Worlds.size());
			const FMaterial& Material       = Resources->ResolveMaterial(Entry.Material);
			const FMaterialHandle MaterialHandle = Entry.Material.IsValid() ? Entry.Material : Resources->GetDefaultMaterial();
			const float CullDistance   = Type.CullDistance * DistanceScale;
			const float ShadowDistance = std::min(Type.ShadowDistance * DistanceScale, CullDistance);
			const std::vector<FFoliageInstance>& Instances = Set.Asset->Instances[TypeIndex];
			for (const FCell& Cell : Entry.Cells)
			{
				const float CellDistance = DistanceToBox(Cell.Bounds, CameraPosition);
				if (CellDistance > CullDistance)
				{
					continue;
				}
				const bool bCellVisible = Frustum.Intersects(Cell.Bounds);
				const bool bCellShadow  = ShadowDistance > 0.0f && CellDistance < ShadowDistance && ShadowCaster && ShadowCaster(Cell.Bounds);
				if (!bCellVisible && !bCellShadow)
				{
					continue;
				}
				++VisitedCells;
				for (const uint32 Index : Cell.Instances)
				{
					const FVector3& Position = Instances[Index].Position;
					const float     Distance = FVector3::Distance(Position, CameraPosition);
					const float     Fade     = FoliageMath::ComputeFade(Distance, CullDistance);
					if (Fade <= 0.01f)
					{
						continue;
					}
					const bool bVisible = bCellVisible && Frustum.Intersects(Entry.Bounds[Index]);
					const bool bShadow  = Distance < ShadowDistance;
					if (!bVisible && !(bShadow && bCellShadow))
					{
						continue;
					}
					FMeshInstance Instance;
					Instance.Mesh           = Mesh;
					Instance.Material       = &Material;
					Instance.MeshHandle     = Entry.Mesh;
					Instance.MaterialHandle = MaterialHandle;
					Instance.World          = Fade < 1.0f ? FoliageMath::MakeWorldMatrix(Instances[Index], Type, Fade) : Entry.Worlds[Index];
					Instance.WorldBounds    = Entry.Bounds[Index];
					Instance.Entity         = Set.Entity;
					Instance.bCastShadow    = bShadow;
					Instance.bFixedLod      = true;
					Instance.bFoliage       = true;
					Instance.PrevWorld      = Instance.World; // 정적 배치: 물체 움직임 없음 (움직임 벡터 = 카메라만, 페이드 축소는 무시)
					if (Mesh->GetLodCount() > 1)
					{
						const float Radius     = Instance.WorldBounds.GetExtent().Length();
						const float ScreenSize = bOrthographic ? LodMath::ComputeOrthographicScreenSize(Radius, Camera.GetOrthoHeight())
						                                       : LodMath::ComputePerspectiveScreenSize(Radius, Distance, TanHalfFov);
						Instance.Lod = LodMath::SelectLod(ScreenSize, Mesh->GetLodScreenSizes(), Mesh->GetLodCount());
					}
					OutInstances.AddExternal(Instance);
					(bVisible ? DrawnInstances : ShadowOnlyInstances) += 1;
				}
			}
		}
	}

	// 오래 쓰지 않은 에셋 캐시 정리 (컴포넌트 삭제/다시 로드)
	for (auto It = Caches.begin(); It != Caches.end();)
	{
		It = FrameCounter - It->second.LastUsedFrame > UnusedFramesBeforeRelease ? Caches.erase(It) : std::next(It);
	}
}

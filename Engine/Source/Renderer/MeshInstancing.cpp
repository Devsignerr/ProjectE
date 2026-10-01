#include "Renderer/MeshInstancing.h"

#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshPalette.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>

void FMeshInstanceList::Add(FEntity Entity, const FTransformComponent& Transform, const FStaticMeshComponent& MeshComponent,
                            const FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes)
{
	++ComponentCount;
	if (!MeshComponent.bVisible)
	{
		return;
	}
	const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent.Mesh);
	if (Mesh == nullptr || (SkinPalettes != nullptr && SkinPalettes->IsCulled(Entity)))
	{
		return; // 가시성 판정에서 빠진 스킨 메시는 정적 메시로 그리면 안 된다
	}

	FMeshInstance& Instance = Instances.emplace_back();
	Instance.Mesh           = Mesh;
	Instance.MeshHandle     = MeshComponent.Mesh;
	Instance.Material       = &Resources.ResolveMaterial(MeshComponent.Material);
	Instance.MaterialHandle = MeshComponent.Material.IsValid() ? MeshComponent.Material : Resources.GetDefaultMaterial();
	Instance.Entity         = Entity;

	// 스킨 메시는 팔레트가 바로 월드로 보낸다 (경계도 팔레트 기준)
	if (const FSkinnedDrawInfo* Skinned = SkinPalettes != nullptr ? SkinPalettes->Find(Entity) : nullptr)
	{
		Instance.bSkinned    = true;
		Instance.BoneOffset  = Skinned->BoneOffset;
		Instance.WorldBounds = Skinned->WorldBounds;
		Instance.World       = FMatrix4x4::Identity;
	}
	else
	{
		Instance.World       = Transform.WorldMatrix;
		Instance.WorldBounds = Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix);
	}
}

void FMeshInstanceList::Gather(FScene& Scene, const FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes)
{
	Instances.clear();
	ComponentCount = 0;
	GpuData        = 0;
	Scene.GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			Add(Entity, Transform, MeshComponent, Resources, SkinPalettes);
		});
}

void FMeshInstanceList::GatherEntities(FScene& Scene, const FResourceManager& Resources, const std::vector<FEntity>& Entities,
                                       const FSkinnedMeshPalette* SkinPalettes)
{
	Instances.clear();
	ComponentCount = 0;
	GpuData        = 0;
	FRegistry& Registry = Scene.GetRegistry();
	for (const FEntity Entity : Entities)
	{
		const FTransformComponent*  Transform = Registry.TryGet<FTransformComponent>(Entity);
		const FStaticMeshComponent* Mesh      = Registry.TryGet<FStaticMeshComponent>(Entity);
		if (Transform != nullptr && Mesh != nullptr)
		{
			Add(Entity, *Transform, *Mesh, Resources, SkinPalettes);
		}
	}
}

void FMeshInstanceList::Upload(FD3D12DynamicUploadBuffer& DynamicBuffer)
{
	// 빈 목록이어도 루트 SRV가 유효한 주소를 가리키게 한 칸은 잡는다
	const size_t                  Count      = std::max<size_t>(Instances.size(), 1);
	const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FInstanceGpuData) * Count, 16);
	FInstanceGpuData*             Data       = static_cast<FInstanceGpuData*>(Allocation.CpuAddress);
	if (Instances.empty())
	{
		std::memset(Data, 0, sizeof(FInstanceGpuData));
	}
	for (size_t Index = 0; Index < Instances.size(); ++Index)
	{
		const FMeshInstance& Instance = Instances[Index];
		FInstanceGpuData     Gpu;
		Gpu.World      = Instance.World;
		Gpu.BoneOffset = Instance.BoneOffset;
		if (!Instance.IsSkinned())
		{
			const FMatrix4x4 Normal = Instance.World.GetInverse().GetTransposed();
			for (int32 Row = 0; Row < 3; ++Row)
			{
				Gpu.NormalMatrix[Row] = FVector4(Normal.M[Row][0], Normal.M[Row][1], Normal.M[Row][2], 0.0f);
			}
		}
		std::memcpy(Data + Index, &Gpu, sizeof(Gpu)); // 업로드 힙(쓰기 결합)은 순차 쓰기만
	}
	GpuData = Allocation.GpuAddress;
}

void DrawDepthBatches(ID3D12GraphicsCommandList* CommandList, const FMeshPassBatches& Batches, const FMeshInstanceList& Instances,
                      ID3D12PipelineState* StaticPipeline, ID3D12PipelineState* SkinnedPipeline, uint32 RootIndex, uint32 DestOffset,
                      uint32& InOutDrawCalls, uint64& InOutTriangles)
{
	bool bSkinnedBound = false;
	for (const FInstanceBatch& Batch : Batches.GetBatches())
	{
		const FMeshInstance& Instance = Instances[Batch.Instance];
		if (Instance.IsSkinned() != bSkinnedBound)
		{
			bSkinnedBound = Instance.IsSkinned();
			CommandList->SetPipelineState(bSkinnedBound ? SkinnedPipeline : StaticPipeline);
		}
		CommandList->SetGraphicsRoot32BitConstant(RootIndex, Batch.First, DestOffset);
		if (bSkinnedBound)
		{
			Instance.Mesh->DrawSkinned(CommandList, Batch.Count);
			InOutTriangles += static_cast<uint64>(Instance.Mesh->GetIndexCount() / 3) * Batch.Count;
		}
		else
		{
			Instance.Mesh->DrawInstanced(CommandList, Batch.Count, Instance.Lod);
			InOutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
		}
		++InOutDrawCalls;
	}
	if (bSkinnedBound)
	{
		CommandList->SetPipelineState(StaticPipeline);
	}
}

void FMeshPassBatches::Reset()
{
	Items.clear();
	Indices.clear();
	Batches.clear();
	IndexBuffer = 0;
}

void FMeshPassBatches::Finalize(FD3D12DynamicUploadBuffer& DynamicBuffer)
{
	InstanceBatching::Build(Items, Indices, Batches);
	const size_t                  Count      = std::max<size_t>(Indices.size(), 1);
	const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(uint32) * Count, 16);
	if (Indices.empty())
	{
		std::memset(Allocation.CpuAddress, 0, sizeof(uint32));
	}
	else
	{
		std::memcpy(Allocation.CpuAddress, Indices.data(), sizeof(uint32) * Indices.size());
	}
	IndexBuffer = Allocation.GpuAddress;
}

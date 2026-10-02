#include "Renderer/MeshInstancing.h"

#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialRender.h"
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
	if (Mesh == nullptr || !Mesh->IsReady() || (SkinPalettes != nullptr && SkinPalettes->IsCulled(Entity))) // 업로드 중인 메시는 그리지 않는다
	{
		return; // 가시성 판정에서 빠진 스킨 메시는 정적 메시로 그리면 안 된다
	}

	FMeshInstance& Instance = Instances.emplace_back();
	Instance.Mesh           = Mesh;
	Instance.MeshHandle     = MeshComponent.Mesh;
	Instance.Material       = &Resources.ResolveMaterial(MeshComponent.Material);
	Instance.MaterialHandle = MeshComponent.Material.IsValid() ? MeshComponent.Material : Resources.GetDefaultMaterial();
	Instance.Entity         = Entity;
	Instance.CopyMaterialState();

	// 스킨 메시는 팔레트가 바로 월드로 보낸다 (경계도 팔레트 기준)
	if (const FSkinnedDrawInfo* Skinned = SkinPalettes != nullptr ? SkinPalettes->Find(Entity) : nullptr)
	{
		Instance.bSkinned       = true;
		Instance.BoneOffset     = Skinned->BoneOffset;
		Instance.PrevBoneOffset = Skinned->PrevBoneOffset;
		Instance.WorldBounds    = Skinned->WorldBounds;
		Instance.World          = FMatrix4x4::Identity;
	}
	else
	{
		Instance.World       = Transform.WorldMatrix;
		Instance.WorldBounds = Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix);
	}
	Instance.PrevWorld = Instance.World;
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
		Gpu.World          = Instance.World;
		Gpu.BoneOffset     = Instance.BoneOffset;
		Gpu.PrevBoneOffset = Instance.PrevBoneOffset;
		Gpu.PrevWorld      = Instance.PrevWorld;
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
                      const FDepthPassBindings& Bindings, uint32& InOutDrawCalls, uint64& InOutTriangles)
{
	ID3D12PipelineState* BoundPipeline = Bindings.Pipelines[0]; // 부른 쪽이 Pipelines[0]을 바인딩해 둔다
	const FMaterial*     BoundMaterial = nullptr;
	for (const FInstanceBatch& Batch : Batches.GetBatches())
	{
		const FMeshInstance& Instance = Instances[Batch.Instance];
		uint32               Variant  = GetDepthVariant(Instance);
		ID3D12PipelineState* Pipeline = Bindings.Pipelines[Variant];
		const bool           bGraph   = (Variant & DepthVariantMasked) != 0 && Instance.Material->IsGraphMaterial();
		if (bGraph)
		{
			// 그래프 머티리얼 Masked: 머티리얼 셰이더별 PSO (실패하면 알파 테스트 없이)
			Pipeline = Bindings.MaterialPipelines != nullptr && Bindings.DynamicBuffer != nullptr
			               ? Bindings.MaterialPipelines->Get(*Instance.Material->Shader, Instance.IsSkinned())
			               : nullptr;
			if (Pipeline == nullptr)
			{
				Variant &= ~DepthVariantMasked;
				Pipeline = Bindings.Pipelines[Variant];
			}
		}
		if (Pipeline != BoundPipeline)
		{
			BoundPipeline = Pipeline;
			CommandList->SetPipelineState(Pipeline);
		}
		if ((Variant & DepthVariantMasked) != 0 && Instance.Material != BoundMaterial)
		{
			BoundMaterial = Instance.Material;
			if (bGraph)
			{
				CommandList->SetGraphicsRootConstantBufferView(Bindings.MaterialConstantRoot,
				                                               MaterialRender::UploadMaterialConstants(*Bindings.DynamicBuffer, *Instance.Material));
				CommandList->SetGraphicsRootDescriptorTable(Bindings.MaterialTextureRoot, Instance.Material->TextureTable.Gpu);
			}
			else
			{
				const float Values[2] = { Instance.Material->Constants.BaseColorFactor.W, Instance.Material->Constants.AlphaCutoff };
				CommandList->SetGraphicsRoot32BitConstants(Bindings.MaskRootIndex, 2, Values, 0);
				CommandList->SetGraphicsRootDescriptorTable(Bindings.MaskTextureRoot, Instance.Material->TextureTable.Gpu);
			}
		}
		CommandList->SetGraphicsRoot32BitConstant(Bindings.InstanceRootIndex, Batch.First, Bindings.InstanceDestOffset);
		if (Instance.IsSkinned())
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
	if (BoundPipeline != Bindings.Pipelines[0])
	{
		CommandList->SetPipelineState(Bindings.Pipelines[0]);
	}
}

void FMeshPassBatches::Reset()
{
	Items.clear();
	Indices.clear();
	Batches.clear();
	IndexBuffer = 0;
}

void FMeshPassBatches::Finalize(FD3D12DynamicUploadBuffer& DynamicBuffer, bool bBackToFront)
{
	if (bBackToFront)
	{
		InstanceBatching::BuildBackToFront(Items, Indices, Batches);
	}
	else
	{
		InstanceBatching::Build(Items, Indices, Batches);
	}
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

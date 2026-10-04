#include "Renderer/MeshInstancing.h"

#include "Core/Jobs/ParallelFor.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialRender.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshPalette.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>

const FStaticMesh* FMeshInstanceList::ResolveDrawable(FEntity Entity, const FStaticMeshComponent& MeshComponent, const FResourceManager& Resources,
                                                     const FSkinnedMeshPalette* SkinPalettes)
{
	if (!MeshComponent.bVisible)
	{
		return nullptr;
	}
	const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent.Mesh);
	if (Mesh == nullptr || !Mesh->IsReady() || (SkinPalettes != nullptr && SkinPalettes->IsCulled(Entity))) // 업로드 중인 메시는 그리지 않는다
	{
		return nullptr; // 가시성 판정에서 빠진 스킨 메시는 정적 메시로 그리면 안 된다
	}
	return Mesh;
}

void FMeshInstanceList::Fill(FMeshInstance& Instance, const FStaticMesh* Mesh, FEntity Entity, const FTransformComponent& Transform,
                             const FStaticMeshComponent& MeshComponent, const FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes)
{
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
	GatheredCount  = 0;
	GpuData        = 0;
	FRegistry&                  Registry     = Scene.GetRegistry();
	const std::vector<FEntity>* ViewEntities = Registry.View<FTransformComponent, FStaticMeshComponent>().GetIterationEntities();
	if (ViewEntities == nullptr || ViewEntities->empty())
	{
		return;
	}
	const TSparseSet<FTransformComponent>*  TransformPool = Registry.TryGetPool<FTransformComponent>();
	const TSparseSet<FStaticMeshComponent>* MeshPool      = Registry.TryGetPool<FStaticMeshComponent>();
	const uint32                            ViewCount     = static_cast<uint32>(ViewEntities->size());

	// 1) 병렬: 뷰 칸마다 그릴 메시 (View<FTransformComponent, FStaticMeshComponent>().Each와 같은 순서·조건)
	GatherSlots.resize(ViewCount);
	FParallel::ParallelFor(ViewCount, 256, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			FGatherSlot&                Slot          = GatherSlots[Index];
			const FEntity               Entity        = (*ViewEntities)[Index];
			const FStaticMeshComponent* MeshComponent = MeshPool->TryGet(Entity);
			Slot.Mesh                                 = nullptr;
			Slot.Output                               = GatherSlotAbsent;
			if (MeshComponent == nullptr || !TransformPool->Contains(Entity))
			{
				continue;
			}
			Slot.Output = 0;
			Slot.Mesh   = ResolveDrawable(Entity, *MeshComponent, Resources, SkinPalettes);
		}
	});

	// 2) 호출 스레드: 출력 번호 (뷰 순서)
	uint32 OutputCount = 0;
	for (FGatherSlot& Slot : GatherSlots)
	{
		if (Slot.Output == GatherSlotAbsent)
		{
			continue;
		}
		++ComponentCount;
		Slot.Output = Slot.Mesh != nullptr ? OutputCount++ : GatherSlotAbsent;
	}
	Instances.resize(OutputCount);
	GatheredCount = OutputCount;

	// 3) 병렬: 인스턴스 채우기 (쓰는 것은 자기 인스턴스뿐)
	FParallel::ParallelFor(ViewCount, 256, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			const FGatherSlot& Slot = GatherSlots[Index];
			if (Slot.Mesh == nullptr)
			{
				continue;
			}
			const FEntity Entity = (*ViewEntities)[Index];
			Fill(Instances[Slot.Output], Slot.Mesh, Entity, TransformPool->Get(Entity), MeshPool->Get(Entity), Resources, SkinPalettes);
		}
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
		const FTransformComponent*  Transform     = Registry.TryGet<FTransformComponent>(Entity);
		const FStaticMeshComponent* MeshComponent = Registry.TryGet<FStaticMeshComponent>(Entity);
		if (Transform == nullptr || MeshComponent == nullptr)
		{
			continue;
		}
		++ComponentCount;
		if (const FStaticMesh* Mesh = ResolveDrawable(Entity, *MeshComponent, Resources, SkinPalettes))
		{
			Fill(Instances.emplace_back(), Mesh, Entity, *Transform, *MeshComponent, Resources, SkinPalettes);
		}
	}
	GatheredCount = static_cast<uint32>(Instances.size());
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
	// 병렬: 구간마다 연속으로 쓴다 (업로드 힙(쓰기 결합)은 읽지 않고 순차 쓰기만)
	FParallel::ParallelFor(static_cast<uint32>(Instances.size()), 256, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
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
			std::memcpy(Data + Index, &Gpu, sizeof(Gpu));
		}
	});
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
		// LOD = 묶음 키 (그림자 캐스케이드 LOD 바이어스는 인스턴스 LOD와 다를 수 있다)
		const uint32 Lod = InstanceBatching::IsUniqueKey(Batch.Key) ? Instance.Lod : InstanceBatching::GetLod(Batch.Key);
		if (Instance.IsSkinned())
		{
			Instance.Mesh->DrawSkinned(CommandList, Batch.Count, Lod);
		}
		else
		{
			Instance.Mesh->DrawInstanced(CommandList, Batch.Count, Lod);
		}
		InOutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Lod).IndexCount / 3) * Batch.Count;
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

#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "Renderer/InstanceBatching.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <vector>

class FD3D12DynamicUploadBuffer;
class FResourceManager;
class FScene;
class FSkinnedMeshPalette;
class FStaticMesh;
struct FMaterial;
struct FStaticMeshComponent;
struct FTransformComponent;

// 메시 인스턴스 하나 (이번 프레임). 스킨 메시는 SkinPalette != 0 (월드 = 팔레트, 인스턴스 버퍼의 행렬은 쓰지 않음)
struct FMeshInstance
{
	const FStaticMesh*        Mesh     = nullptr;
	const FMaterial*          Material = nullptr;
	FMeshHandle               MeshHandle;
	FMaterialHandle           MaterialHandle; // 무효 핸들이면 기본 머티리얼 핸들로 바뀌어 있다
	FBox                      WorldBounds;
	FMatrix4x4                World;
	FEntity                   Entity;
	D3D12_GPU_VIRTUAL_ADDRESS SkinPalette = 0;
	uint32                    Lod         = 0; // 메인 카메라 화면 크기로 고른 LOD (그림자 패스도 같은 값)

	bool IsSkinned() const { return SkinPalette != 0; }
};

// 프레임 단위 메시 인스턴스 목록: 씬의 보이는 메시를 한 번 모아(컬링 전) 월드 행렬을 GPU 구조화 버퍼로 올린다.
// 모든 패스(메인/방향광 그림자/로컬 그림자/에디터 아웃라인)가 같은 목록을 각자 컬링해 FMeshPassBatches로 묶어 그린다.
// 인스턴스 번호 = 목록 인덱스. GPU 데이터는 Upload한 프레임 안에서만 유효(동적 업로드 버퍼)
class FMeshInstanceList
{
public:
	// 씬의 모든 FStaticMeshComponent (보이고 메시가 있는 것). SkinPalettes에 있는 엔티티는 스킨 인스턴스
	void Gather(FScene& Scene, const FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes);
	// 지정한 엔티티만 (아웃라인 등)
	void GatherEntities(FScene& Scene, const FResourceManager& Resources, const std::vector<FEntity>& Entities, const FSkinnedMeshPalette* SkinPalettes);
	// 정적 인스턴스의 월드/법선 행렬을 올린다 (Gather 뒤 한 번)
	void Upload(FD3D12DynamicUploadBuffer& DynamicBuffer);

	const std::vector<FMeshInstance>& GetInstances() const { return Instances; }
	std::vector<FMeshInstance>&       GetInstances() { return Instances; } // LOD 지정용
	const FMeshInstance&              operator[](uint32 Index) const { return Instances[Index]; }
	uint32                            GetCount() const { return static_cast<uint32>(Instances.size()); }
	uint32                            GetComponentCount() const { return ComponentCount; } // 보이지 않는 것 포함 (통계)
	D3D12_GPU_VIRTUAL_ADDRESS         GetGpuData() const { return GpuData; }

private:
	void Add(FEntity Entity, const FTransformComponent& Transform, const FStaticMeshComponent& MeshComponent,
	         const FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes);

	std::vector<FMeshInstance> Instances;
	uint32                     ComponentCount = 0;
	D3D12_GPU_VIRTUAL_ADDRESS  GpuData        = 0;
};

// 패스 하나의 묶음: Add(키, 깊이, 인스턴스) → Finalize(정렬·묶음·인스턴스 번호 목록 업로드) → 묶음마다 DrawIndexedInstanced
class FMeshPassBatches
{
public:
	void Reset();
	void Add(uint64 Key, float Depth, uint32 Instance) { Items.push_back({ Key, Depth, Instance }); }
	void Finalize(FD3D12DynamicUploadBuffer& DynamicBuffer);

	const std::vector<FInstanceBatch>& GetBatches() const { return Batches; }
	const std::vector<uint32>&         GetIndices() const { return Indices; }
	D3D12_GPU_VIRTUAL_ADDRESS          GetIndexBuffer() const { return IndexBuffer; } // StructuredBuffer<uint> (t14)
	bool                               IsEmpty() const { return Batches.empty(); }

private:
	std::vector<FInstanceSortItem> Items;
	std::vector<uint32>            Indices;
	std::vector<FInstanceBatch>    Batches;
	D3D12_GPU_VIRTUAL_ADDRESS      IndexBuffer = 0;
};

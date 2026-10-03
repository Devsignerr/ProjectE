#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "Renderer/InstanceBatching.h"
#include "Renderer/Material.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <vector>

class FD3D12DynamicUploadBuffer;
class FMaterialDepthPipelines;
class FResourceManager;
class FScene;
class FSkinnedMeshPalette;
class FStaticMesh;
struct FStaticMeshComponent;
struct FTransformComponent;

// 메시 인스턴스 하나 (이번 프레임). 스킨 메시는 bSkinned (월드 = 프레임 팔레트[BoneOffset..], 인스턴스 버퍼의 행렬은 쓰지 않음)
struct FMeshInstance
{
	const FStaticMesh*        Mesh     = nullptr;
	const FMaterial*          Material = nullptr;
	FMeshHandle               MeshHandle;
	FMaterialHandle           MaterialHandle; // 무효 핸들이면 기본 머티리얼 핸들로 바뀌어 있다
	FBox                      WorldBounds;
	FMatrix4x4                World;
	FMatrix4x4                PrevWorld; // 이전 프레임 월드 (움직임 벡터, FSceneRenderer가 엔티티 이력으로 채움. 기본 = World)
	FEntity                   Entity;
	uint32                    BoneOffset  = 0; // 스킨: 프레임 팔레트 버퍼(FSkinnedMeshPalette::GetGpuData, t15) 안 첫 본
	uint32                    PrevBoneOffset = 0; // 스킨: 같은 버퍼 안 이전 프레임 팔레트 (이력 없으면 BoneOffset)
	uint32                    Lod         = 0; // 메인 카메라 화면 크기로 고른 LOD (그림자 패스도 같은 값)
	bool                      bSkinned    = false;
	bool                      bCastShadow = true;  // false면 그림자 패스(방향광/로컬)에서 뺀다 (폴리지 그림자 거리)
	bool                      bFixedLod   = false; // true면 씬 렌더러 LOD 선택이 건드리지 않는다 (폴리지가 직접 고름)
	bool                      bFoliage    = false; // 폴리지 인스턴스 (레이 트레이싱 인스턴스 마스크 — r.RayTracing.Foliage로 뺄 수 있다)
	// 방향광 그림자 캐시의 정적 캐스터 (ShadowCacheMath.h 머리 주석): 씬 렌더러가 위치가 일정 프레임 그대로인 비스킨 인스턴스에 켠다.
	// 기본 false = 동적 (매 프레임 그림). AddExternal로 넣는 쪽은 배치가 고정이면 직접 켠다 (폴리지)
	bool                      bShadowStatic = false;
	EMaterialBlendMode        BlendMode   = EMaterialBlendMode::Opaque; // 머티리얼 렌더 상태 사본 (Gather/AddExternal이 Material에서 채움)
	bool                      bTwoSided   = false;

	bool IsSkinned() const { return bSkinned; }
	bool IsMasked() const { return BlendMode == EMaterialBlendMode::Masked; }
	bool IsTranslucent() const { return MaterialRender::IsTranslucent(BlendMode); }
	// 그림자 패스(방향광/로컬)에 넣는가: 반투명은 그림자를 드리우지 않는다
	bool CastsShadow() const { return bCastShadow && !IsTranslucent(); }
	// 메시 패스 PSO 변형 (MaterialRender::MakeVariant — 불투명 패스는 Masked, 반투명 패스는 Additive 비트)
	uint32 GetPipelineVariant() const
	{
		const bool bVariantBit = IsTranslucent() ? BlendMode == EMaterialBlendMode::Additive : IsMasked();
		return MaterialRender::MakeVariant(bSkinned, bVariantBit, bTwoSided);
	}
	// Material의 블렌드 모드/양면을 사본으로 옮긴다
	void CopyMaterialState()
	{
		BlendMode = Material != nullptr ? Material->BlendMode : EMaterialBlendMode::Opaque;
		bTwoSided = Material != nullptr && Material->bTwoSided;
	}
};

// 프레임 단위 메시 인스턴스 목록: 씬의 보이는 메시를 한 번 모아(컬링 전) 월드 행렬을 GPU 구조화 버퍼로 올린다.
// 모든 패스(메인/방향광 그림자/로컬 그림자/에디터 아웃라인)가 같은 목록을 각자 컬링해 FMeshPassBatches로 묶어 그린다.
// 인스턴스 번호 = 목록 인덱스. GPU 데이터는 Upload한 프레임 안에서만 유효(동적 업로드 버퍼)
class FMeshInstanceList
{
public:
	// 씬의 모든 FStaticMeshComponent (보이고 메시가 있는 것). SkinPalettes에 있는 엔티티는 스킨 인스턴스,
	// SkinPalettes가 가시성 판정으로 뺀 엔티티(IsCulled)는 넣지 않는다
	void Gather(FScene& Scene, const FResourceManager& Resources, const FSkinnedMeshPalette* SkinPalettes);
	// 지정한 엔티티만 (아웃라인 등)
	void GatherEntities(FScene& Scene, const FResourceManager& Resources, const std::vector<FEntity>& Entities, const FSkinnedMeshPalette* SkinPalettes);
	// 정적 인스턴스의 월드/법선 행렬, 스킨 인스턴스의 본 오프셋을 올린다 (Gather 뒤 한 번)
	void Upload(FD3D12DynamicUploadBuffer& DynamicBuffer);
	// 씬 컴포넌트 밖 인스턴스 추가 (Gather 뒤, Upload 전 — 폴리지). Mesh/Material/핸들/World/WorldBounds/Entity를 채워 넘긴다
	void AddExternal(const FMeshInstance& Instance)
	{
		Instances.push_back(Instance);
		Instances.back().CopyMaterialState();
	}

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

// 메인/사전/반투명 패스 묶음 키: PSO 변형(GetPipelineVariant) | 머티리얼 | 메시 | LOD
inline uint64 MakeMainBatchKey(const FMeshInstance& Instance)
{
	return InstanceBatching::MakeKey(Instance.GetPipelineVariant(), Instance.MaterialHandle.Index, Instance.MeshHandle.Index,
	                                 Instance.Lod);
}

// 깊이 전용 패스(그림자) PSO 변형: bit0 = 스킨, bit1 = Masked (알파 테스트 픽셀 셰이더)
constexpr uint32 DepthVariantSkinned = 1u;
constexpr uint32 DepthVariantMasked  = 2u;
constexpr uint32 DepthVariantCount   = 4u;
inline uint32 GetDepthVariant(const FMeshInstance& Instance)
{
	return (Instance.IsSkinned() ? DepthVariantSkinned : 0u) | (Instance.IsMasked() ? DepthVariantMasked : 0u);
}

// 깊이 전용 패스(그림자) 묶음 키: 변형 | 머티리얼(Masked만 — 나머지는 머티리얼 무관) | 메시 | LOD (스킨 포함)
// Lod = 그릴 LOD (방향광 그림자는 캐스케이드 LOD 바이어스를 적용한 값 — DrawDepthBatches가 키의 LOD로 그린다)
inline uint64 MakeDepthBatchKey(const FMeshInstance& Instance, uint32 Lod)
{
	return InstanceBatching::MakeKey(GetDepthVariant(Instance), Instance.IsMasked() ? Instance.MaterialHandle.Index : 0u, Instance.MeshHandle.Index,
	                                 Lod);
}
inline uint64 MakeDepthBatchKey(const FMeshInstance& Instance)
{
	return MakeDepthBatchKey(Instance, Instance.Lod);
}

class FMeshPassBatches;

// 깊이 패스 PSO/루트 인자 (그림자 공용). Masked 변형은 픽셀 셰이더가 베이스 컬러 알파로 잘라낸다 (Shadow.hlsl ShadowMaskedPS)
struct FDepthPassBindings
{
	ID3D12PipelineState* Pipelines[DepthVariantCount] = {}; // GetDepthVariant 순서
	uint32               InstanceRootIndex  = 0; // 루트 상수: InstanceDestOffset 칸에 묶음 시작 위치
	uint32               InstanceDestOffset = 0;
	uint32               MaskRootIndex      = 0; // 루트 상수 2개 (b1: 베이스 컬러 알파 팩터, 알파 컷오프)
	uint32               MaskTextureRoot    = 0; // 디스크립터 테이블 t0 (머티리얼 텍스처 테이블 첫 칸 = 베이스 컬러)
	// 그래프 머티리얼 Masked (Shadow.hlsl ShadowMaterialPS): PSO는 머티리얼 셰이더별, b2 상수 + 공간 2 텍스처 테이블.
	// MaterialPipelines가 없거나 PSO가 실패하면 알파 테스트 없는 변형으로 그린다
	FMaterialDepthPipelines*   MaterialPipelines    = nullptr;
	FD3D12DynamicUploadBuffer* DynamicBuffer        = nullptr;
	uint32                     MaterialConstantRoot = 0; // 루트 CBV b2
	uint32                     MaterialTextureRoot  = 0; // 디스크립터 테이블 공간 2 t0~
};

// 깊이 패스 묶음 드로우: 묶음마다 인스턴싱 드로우. 묶음은 키 순(변형 -> 머티리얼)이라 PSO는 변형이 바뀔 때만,
// 마스크 인자는 Masked 머티리얼이 바뀔 때만 바꾼다. 부른 쪽이 Pipelines[0]을 바인딩해 두고, 끝나면 다시 Pipelines[0]이 바인딩된 상태
void DrawDepthBatches(ID3D12GraphicsCommandList* CommandList, const FMeshPassBatches& Batches, const FMeshInstanceList& Instances,
                      const FDepthPassBindings& Bindings, uint32& InOutDrawCalls, uint64& InOutTriangles);

// 패스 하나의 묶음: Add(키, 깊이, 인스턴스) → Finalize(정렬·묶음·인스턴스 번호 목록 업로드) → 묶음마다 DrawIndexedInstanced
class FMeshPassBatches
{
public:
	void Reset();
	void Add(uint64 Key, float Depth, uint32 Instance) { Items.push_back({ Key, Depth, Instance }); }
	// bBackToFront: 반투명 패스 — 먼 것부터 그리는 순서 (InstanceBatching::BuildBackToFront)
	void Finalize(FD3D12DynamicUploadBuffer& DynamicBuffer, bool bBackToFront = false);

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

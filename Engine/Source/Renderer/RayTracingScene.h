#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/RayTracingMath.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Scene/ResourceHandles.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FD3D12RHI;
class FMeshInstanceList;
class FResourceManager;
class FScene;
struct FMeshInstance;
class FShaderLibrary;
class FStaticMesh;
struct FMaterial;
struct FMaterialShader;
struct FTerrainData;
struct FTerrainRayTracingInput;

// ---- GPU 구조 (RayTracingCommon.hlsli와 1:1)

// 지오메트리 하나의 기하/머티리얼 정보 (StructuredBuffer, 번호 = TLAS InstanceID + GeometryIndex — 정적/지형은 지오메트리 1개). 정점/인덱스는 바인드리스 힙 칸 (ByteAddressBuffer)
struct FRayTracingInstanceGpu
{
	uint32 VertexBuffer = 0; // 힙 칸: FVertex 64B 정점 (스킨 = 이번 프레임 월드 공간 스키닝 결과)
	uint32 IndexBuffer  = 0; // 힙 칸: uint32 인덱스
	uint32 FirstIndex   = 0; // LOD 시작 (인덱스 단위) — 삼각형 i = 인덱스 [FirstIndex + 3i, +3)
	uint32 Material     = 0; // 머티리얼 표 번호
	uint32 Flags        = 0; // RayTracingMath::InstanceInfo*
	uint32 BaseVertex   = 0; // 정점 버퍼 안 첫 정점 (스킨 = 정점 풀 하위 할당 위치, 정적 = 0) — 인덱스 값에 더한다
	uint32 Padding[2]   = {};
};
static_assert(sizeof(FRayTracingInstanceGpu) == 32);

// 히트 머티리얼 (고정 PBR 상수 + 텍스처 테이블 힙 칸 — MaterialDefault.hlsli를 E_MATERIAL_CUSTOM_RESOURCES로 바인드리스 평가).
// 그래프 머티리얼: GraphSlot ≥ 1이면 RT 셰이더 그래프 변형(E_RT_GRAPH_MATERIALS)이 생성 함수(슬롯별)를 부른다 — 상수 = 그래프 파라미터 버퍼
// [GraphParams] = 머리(시간, 알파 컷오프) + [GraphParams + 1 ..] = 파라미터, 텍스처 = GraphTextureTable. 변형이 없으면(컴파일 실패/끔)
// 위 고정 PBR 필드(회색 근사 + 기본 텍스처)로 평가한다
struct FRayTracingMaterialGpu
{
	FVector4 BaseColorFactor   = FVector4::OneVector;
	FVector3 EmissiveFactor    = FVector3::ZeroVector;
	float    Metallic          = 0.0f;
	float    Roughness         = 0.5f;
	float    NormalScale       = 1.0f;
	float    OcclusionStrength = 1.0f;
	float    AlphaCutoff       = 0.5f;
	uint32   TextureTable      = 0; // 힙 칸 (연속 5칸: 베이스/금속거칠기/노멀/AO/발광)
	uint32   Flags             = 0; // MaterialFlag*
	uint32   GraphSlot         = 0; // 0 = 고정 PBR, 1~ = 이번 프레임 그래프 셰이더 슬롯 (해시 순)
	uint32   GraphParams       = 0; // 그래프 파라미터 버퍼(float4) 시작
	uint32   GraphTextureTable = 0; // 그래프 텍스처 테이블 힙 칸 (FMaterial::TextureTable)
	uint32   Padding[3]        = {};

	static constexpr uint32 MaterialFlagGraph  = 1u << 0; // 그래프 머티리얼 (변형이 없으면 회색 근사)
	static constexpr uint32 MaterialFlagMasked = 1u << 1;
};
static_assert(sizeof(FRayTracingMaterialGpu) == 80);

// 이번 프레임 그래프 머티리얼 RT 변형: 장면 TLAS에 들어간 그래프 셰이더(해시 순 = 슬롯 1~)의 생성 함수를 이어 붙인 가상 포함 파일.
//   Key = 해시 목록의 해시 (같은 집합이면 같은 변형 — 셰이더 라이브러리 캐시/쿠킹 파일·PSO 공유). Key 0 = 그래프 머티리얼 없음
struct FRayTracingGraphVariant
{
	uint64      Key = 0;
	std::string Source; // RayTracingGraphMaterials.generated.hlsli
	uint32      SlotCount = 0;
};

struct FRayTracingSceneOptions
{
	FVector3 CameraPosition;
	bool     bSkinned             = true;     // 스킨 메시 (계산 셰이더 스키닝 + BLAS 갱신)
	bool     bFoliage             = true;     // 폴리지 인스턴스
	float    SkinnedMaxDistance   = 5000.0f;  // cm, 카메라에서 이보다 먼 스킨 모델은 TLAS에서 뺀다 (갱신 비용 상한)
	float    SkinnedRefitDistance = 1500.0f;  // cm, 이 안의 스킨 모델은 매 프레임 갱신, 밖은 거리에 따라 2~SkinnedRefitInterval 프레임마다
	uint32   SkinnedRefitInterval = 4;        // 먼 스킨 모델의 최대 갱신 주기 (1 = 모두 매 프레임)
	uint32   MaxBuildsPerFrame    = 32;       // 새 BLAS 빌드 수 상한 (끊김 방지 — 나머지는 다음 프레임, 그동안 TLAS에 없음)
	uint64   MaxBuildTriangles    = 2000000;  // 프레임당 새 BLAS 삼각형 상한
	bool     bCompaction          = true;     // 정적 BLAS 압축 (빌드 → 몇 프레임 뒤 크기 읽기 → 복사)
	bool     bGraphMaterials      = true;     // 그래프 머티리얼 히트를 생성 함수로 (끄면 회색 근사)
	bool     bTerrain             = true;     // 지형 높이장 타일 BLAS
	uint32   MaxTerrainVertices   = 131072;   // 지형 하나의 RT 정점 상한 → 간격(2의 거듭제곱 셀)을 고른다 (RayTracingMath::SelectTerrainStep)
	uint32   MaxGraphSlots        = 32;       // 한 변형에 넣을 그래프 셰이더 상한 (넘치면 나머지는 회색 근사)
};

struct FRayTracingSceneStats
{
	uint32 StaticBlas        = 0; // 캐시의 정적 BLAS 수
	uint32 SkinnedBlas       = 0; // 스킨 모델 BLAS (모델마다 지오메트리 여러 개)
	uint32 SkinnedPrimitives = 0; // 이번 프레임 TLAS에 든 스킨 프리미티브 (지오메트리)
	uint32 SkinnedRefitSkipped = 0; // 갱신 주기 때문에 이번 프레임 갱신을 건너뛴 스킨 모델
	uint32 TerrainTiles      = 0; // 지형 타일 BLAS (빌드된 것)
	uint64 TerrainVertexBytes = 0;
	uint64 BlasBytes         = 0; // 정적 + 스킨 BLAS 버퍼
	uint64 SkinnedVertexBytes = 0; // 스키닝 결과 정점 풀
	uint64 TlasBytes         = 0;
	uint64 ScratchBytes      = 0;
	uint32 TlasInstances     = 0;
	uint32 BuiltThisFrame    = 0; // 새 BLAS
	uint32 RefitThisFrame    = 0; // 스킨 갱신
	uint32 CompactedThisFrame = 0;
	uint32 PendingBuilds     = 0; // 상한 때문에 미룬 인스턴스
	uint64 CompactionSavedBytes = 0; // 지금까지 압축으로 줄인 바이트
	float  PrepareCpuMs      = 0.0f;
};

// 레이 트레이싱 씬 (Phase 50): 프레임 메시 인스턴스 목록 → BLAS 캐시 + 프레임 TLAS + 히트 정보 버퍼.
//
// BLAS
//   정적 메시: (메시 핸들, LOD) 키로 캐시 (RayTracingMath::FBlasKey). LOD = 래스터와 같은 인스턴스 LOD(RT 그림자가 래스터 표면과 같은 면 —
//     자기 그림자 여드름 방지). 비동기 업로드가 끝난 메시(IsReady)만, 프레임당 빌드 상한(Options) — 넘친 인스턴스는 다음 프레임.
//     PREFER_FAST_TRACE | ALLOW_COMPACTION → 빌드 프레임 슬롯의 리드백으로 압축 크기를 읽고(같은 슬롯이 돌아오면 GPU 완료가 보장됨)
//     압축 복사 → 이전 버퍼는 지연 해제. BlasEvictFrames 동안 TLAS에 안 들어가면 해제(+ 바인드리스 SRV 지연 반환).
//   스킨 메시: 모델(스켈레톤 = FSkinComponent::Joints[0], + 양면·그림자 여부) 하나 = ALLOW_UPDATE BLAS 하나, 프리미티브마다 지오메트리 하나
//     (지오메트리별 OPAQUE 플래그 — Masked만 후보). 히트 정보 = InstanceID(첫 정보) + GeometryIndex. 월드 공간 정점은 정점 풀 하나에
//     프리미티브별 하위 할당(계산 셰이더 RayTracingSkinning.hlsl — 프레임 팔레트 t15, 정보 BaseVertex), BLAS도 BLAS 풀 하나에 하위 할당
//     → 렌더 그래프 리소스는 풀 2개뿐(엔티티 수와 무관). 멤버(순서·정점 위치·불투명) 서명이 같으면 갱신(refit), 다르면 다시 빌드.
//     카메라에서 SkinnedMaxDistance 안 + 팔레트 가시성 컬링을 통과한 것만 (FMeshInstanceList가 이미 뺀다). SkinnedRefitDistance 밖은
//     RayTracingMath::GetSkinnedRefitInterval 주기로 엇갈려 갱신 — 갱신하지 않는 프레임은 스키닝도 건너뛴다(정점·BLAS가 같은 시점).
//     풀이 커지면 새 버퍼(기존 위치 유지) + 모든 스킨 BLAS 다시 빌드, 해제한 범위는 FrameCount 프레임 뒤에 재사용.
//   지형/폴리지: 폴리지는 정적 메시 인스턴스 그대로 (마스크 MaskFoliage), 지형 높이장 BLAS는 후속.
// TLAS
//   프레임마다 다시 빌드 (PREFER_FAST_TRACE, 프레임 슬롯별 버퍼). InstanceID = 정보 버퍼 번호, 마스크 = RayTracingMath::GetInstanceMask
//   (종류 + 그림자 캐스터), 플래그 = ComputeInstanceFlags (불투명 FORCE_OPAQUE / Masked FORCE_NON_OPAQUE / 양면 컬링 끔 / 반사 행렬),
//   히트 그룹 오프셋 = ComputeHitGroupOffset(셰이딩 슬롯 — 지금 0, 상태 객체용 예약). 반투명은 넣지 않는다.
// 렌더 그래프: 스키닝(계산) → BLAS 빌드/갱신 + 압축 크기 기록 → 압축 복사 → 리드백 복사 → TLAS 빌드. 가속 구조는 AccelStruct* 접근으로 선언
//   (RenderGraphCompiler 규칙 2.5), 스키닝 정점은 Uav → SrvNonPixel(BLAS 입력) → SrvPixel(추적 패스).
//   정적 메시 정점/인덱스 버퍼는 COMMON 상태 버퍼의 암시적 승격으로 읽는다 (그래프 밖).
//
// ---- Phase 51 DDGI가 쓸 API
//   GetTlasAddress()/GetInstanceBuffer()/GetMaterialBuffer() + FRayTracingPassRoot(RayTracingPasses.h)로 루트를 묶고,
//   셰이더는 RayTracingCommon.hlsli(TraceClosestHit/TraceShadowRay, LoadHitSurface — 정점 보간 + 바인드리스 머티리얼 평가)와
//   RayTracingLighting.hlsli(EvaluateHitLighting — 방향광 + RT 그림자 광선, 로컬 라이트, IBL/캡처, 발광)를 include한다.
//   추적 패스는 AddBuildPasses가 돌려준 TLAS 참조와 DeclareTraceReads를 선언한다. 상태 객체(TraceRay) 경로는 히트 그룹 오프셋이 이미 채워져 있다.
class FRayTracingScene
{
public:
	~FRayTracingScene();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);
	bool IsSupported() const { return Device5 != nullptr; }

	// 이번 프레임 TLAS 준비 (CPU, 등록 시점): 압축 크기 읽기 → 수명 정리 → 인스턴스 → BLAS 요청/스킨 갱신 + TLAS 인스턴스 + 정보 버퍼 업로드
	//   Scene: 스킨 프리미티브를 모델로 묶는 데 쓴다 (FSkinComponent::Joints[0]) — 없으면 프리미티브마다 BLAS
	void Prepare(const FMeshInstanceList& Instances, const FResourceManager& Resources, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
	             const FRayTracingSceneOptions& Options, const std::vector<FTerrainRayTracingInput>* Terrains = nullptr, const FScene* Scene = nullptr);
	// 그래프 패스 등록. 반환 = TLAS 참조 (추적 패스가 Read(…, AccelStructRead))
	FRGResourceRef AddBuildPasses(FRenderGraph& Graph, int32 Timer);
	// 추적 패스가 읽는 리소스 선언: TLAS + 이번 프레임 스킨 정점 버퍼 (바인드리스로 읽음)
	void DeclareTraceReads(FRenderGraph::FPassBuilder& Pass, FRGResourceRef Tlas) const;

	D3D12_GPU_VIRTUAL_ADDRESS    GetTlasAddress() const { return TlasAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS    GetInstanceBuffer() const { return InstanceBufferAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS    GetMaterialBuffer() const { return MaterialBufferAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS    GetGraphParamBuffer() const { return GraphParamAddress; }
	const FRayTracingGraphVariant& GetGraphVariant() const { return GraphVariant; }
	uint32                       GetInstanceCount() const { return static_cast<uint32>(TlasDescs.size()); }
	const FRayTracingSceneStats& GetStats() const { return Stats; }

private:
	enum class EBlasState : uint8
	{
		Pending,    // 이번 프레임 빌드 예약
		Built,      // 빌드됨, 압축 크기 대기 (CompactionSlot 리드백)
		Compacted,  // 최종 (압축됐거나 압축 안 함)
	};
	struct FStaticBlas
	{
		RayTracingMath::FBlasKey  Key;
		const FStaticMesh*        Mesh = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS SourceVertexAddress = 0; // 메시가 다시 만들어지면 (같은 핸들, 다른 버퍼) 다시 빌드
		ComPtr<ID3D12Resource>    Blas;
		uint64                    BlasSize      = 0;
		ComPtr<ID3D12Resource>    CompactBlas;  // 이번 프레임 압축 복사 대상 (복사 패스 뒤 Blas와 교체)
		uint64                    CompactSize   = 0;
		uint32                    FirstIndex    = 0;
		uint32                    IndexCount    = 0;
		uint64                    ScratchSize   = 0;
		EBlasState                State         = EBlasState::Pending;
		uint32                    CompactionSlot  = 0; // 빌드한 프레임 슬롯
		uint32                    CompactionIndex = 0; // 그 슬롯 압축 크기 버퍼 안 번호
		uint64                    BuildFrame      = 0;
		uint64                    LastUsedFrame   = 0;
		FD3D12DescriptorHandle    VertexSrv;
		FD3D12DescriptorHandle    IndexSrv;
		// 지형 타일: 정점 버퍼를 항목이 소유 (업로드 힙 — 다시 만들 때 새 버퍼, 이전 것은 지연 해제). 인덱스는 지형 공용
		ComPtr<ID3D12Resource>    OwnedVertices;
		uint32                    VertexCount = 0;
		bool                      bDirty      = true;
	};
	// 지형 하나의 높이장 타일 BLAS (Phase 50): 타일 = 청크 크기 정사각형, 정점 간격 Step 셀(정점 수 상한), 위치는 지형 원점 기준(TLAS 변환 = 이동)
	struct FTerrainBlas
	{
		const FTerrainData*                       Data       = nullptr;
		uint32                                    Resolution = 0;
		uint32                                    Step       = 1;
		uint32                                    TileCells  = 64;
		uint32                                    TilesPerSide = 0;
		uint64                                    SeenCounter  = 0;
		FVector2                                  CellSize;
		float                                     HeightScale = 0.0f;
		float                                     Tiling      = 0.0f;
		FVector4                                  Color;
		FVector3                                  Origin; // UV(월드 XY × Tiling) 기준 — 바뀌면 전부 다시
		ComPtr<ID3D12Resource>                    Indices; // 타일 공용 (업로드 힙)
		FD3D12DescriptorHandle                    IndexSrv;
		uint32                                    IndexCount = 0;
		std::vector<std::unique_ptr<FStaticBlas>> Tiles;
		uint64                                    LastUsedFrame = 0;
	};
	// 스킨 프리미티브 (엔티티별): 정점 풀 안 월드 공간 정점 범위
	struct FSkinnedPrimitive
	{
		FMeshHandle        MeshHandle;
		const FStaticMesh* Mesh          = nullptr;
		uint32             VertexCount   = 0;
		uint32             IndexCount    = 0;
		uint64             VertexOffset  = RayTracingMath::FRangeAllocator::InvalidOffset; // 바이트 (정점 풀)
		uint64             VertexBytes   = 0;
		uint64             LastUsedFrame = 0;
		bool               bActive       = false; // 이번 프레임 (같은 엔티티 중복 방지)
	};
	// 스킨 모델 BLAS의 지오메트리 하나 (서명: 바뀌면 다시 빌드)
	struct FSkinnedGeometryKey
	{
		uint64                    Entity       = 0;
		const FStaticMesh*        Mesh         = nullptr;
		uint64                    VertexOffset = 0;
		D3D12_GPU_VIRTUAL_ADDRESS IndexAddress = 0;
		uint32                    VertexCount  = 0;
		uint32                    IndexCount   = 0;
		bool                      bOpaque      = true;

		bool operator==(const FSkinnedGeometryKey& Other) const = default;
	};
	// 스킨 모델 (묶음 키별): BLAS 풀 안 BLAS 하나
	struct FSkinnedGroup
	{
		std::vector<FSkinnedGeometryKey> Geometries; // 지금 BLAS를 이룬 멤버 (순서 = 지오메트리 번호)
		uint64 BlasOffset     = RayTracingMath::FRangeAllocator::InvalidOffset; // 바이트 (BLAS 풀)
		uint64 BlasSize       = 0;
		uint64 ScratchSize    = 0; // 빌드/갱신 중 큰 값
		uint32 Phase          = 0; // 갱신 주기 위상
		bool   bBuilt         = false;
		uint64 LastRefitFrame = 0;
		uint64 LastUsedFrame  = 0;
	};
	// 하위 할당 풀 (버퍼 하나)
	struct FSkinnedPool
	{
		ComPtr<ID3D12Resource>          Buffer;
		RayTracingMath::FRangeAllocator Allocator;
	};
	struct FPendingRangeFree
	{
		FSkinnedPool* Pool   = nullptr;
		uint64        Offset = 0;
		uint64        Size   = 0;
		uint64        Frame  = 0; // 해제한 프레임 (FrameCount 프레임 뒤 재사용)
	};
	// 메시 인덱스 버퍼 SRV (스킨 지오메트리 공용 — 리소스를 붙잡아 주소 재사용을 막는다)
	struct FIndexSrv
	{
		ComPtr<ID3D12Resource> Resource;
		FD3D12DescriptorHandle Srv;
		uint64                 LastUsedFrame = 0;
	};
	// 이번 프레임 스킨 후보 (Prepare 안)
	struct FFrameSkinItem
	{
		const FMeshInstance* Instance   = nullptr;
		uint64               GroupKey   = 0;
		uint32               GroupOrder = 0; // 묶음 첫 등장 순서
	};
	// 프레임 슬롯별 (같은 슬롯이 돌아오면 그 프레임 GPU 작업이 끝나 있다 — RHI BeginFrame 대기)
	struct FSlot
	{
		ComPtr<ID3D12Resource> Tlas;
		uint64                 TlasCapacity = 0;
		ComPtr<ID3D12Resource> Scratch;     // BLAS 빌드/갱신 + TLAS 빌드 (영역 나눔), 그래프 밖 상태 COMMON
		uint64                 ScratchCapacity = 0;
		ComPtr<ID3D12Resource> PostbuildInfo; // 압축 크기 (UINT64 × MaxPostbuild), 그래프 밖 상태 COMMON
		ComPtr<ID3D12Resource> Readback;      // 위의 리드백 사본
		uint32                 PostbuildCount = 0; // 이 슬롯이 마지막으로 기록한 압축 크기 수
	};
	static constexpr uint32 MaxPostbuild = 256;

	// 이번 프레임 작업
	struct FBuildOp
	{
		ID3D12Resource*                                     Dest          = nullptr; // 그래프 선언용 (스킨 = BLAS 풀)
		D3D12_GPU_VIRTUAL_ADDRESS                           DestAddress   = 0;
		D3D12_GPU_VIRTUAL_ADDRESS                           Source        = 0; // 갱신이면 = DestAddress
		uint32                                              FirstGeometry = 0; // BuildGeometries 안
		uint32                                              GeometryCount = 1;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
		uint64                                              ScratchOffset = 0;
		int32                                               PostbuildIndex = -1; // 압축 크기 기록 칸 (-1 = 없음)
		bool                                                bScratchBarrier = false; // 스크래치 예산을 넘어 처음부터 재사용 — 이 빌드 앞에 UAV 배리어
	};
	struct FSkinOp
	{
		uint64                    OutputOffset = 0; // 정점 풀 안 바이트
		D3D12_GPU_VIRTUAL_ADDRESS BaseVertices = 0;
		D3D12_GPU_VIRTUAL_ADDRESS SkinVertices = 0;
		uint32                    VertexCount  = 0;
		uint32                    BoneOffset   = 0;
	};
	struct FCompactOp
	{
		ID3D12Resource* Source = nullptr;
		ID3D12Resource* Dest   = nullptr;
	};

	void           ProcessCompactionReadback(FSlot& Slot);
	void           ProcessCompaction(FStaticBlas& Entry, const uint64* Sizes, uint32 SizeCount);
	FTerrainBlas*  EnsureTerrain(const FTerrainRayTracingInput& Input, const FRayTracingSceneOptions& Options);
	bool           WriteTerrainTile(FTerrainBlas& Terrain, uint32 TileX, uint32 TileY, FStaticBlas& Tile);
	void           ReleaseTerrain(FTerrainBlas& Terrain);
	void           EvictUnused();
	FStaticBlas*   FindOrCreateStatic(const FStaticMesh& Mesh, FMeshHandle Handle, uint32 Lod);
	FSkinnedPrimitive* FindOrCreateSkinnedPrimitive(FEntity Entity, const FStaticMesh& Mesh, FMeshHandle Handle);
	uint64         AllocateSkinned(FSkinnedPool& Pool, uint64 Size, bool bVertexPool);
	void           FreeSkinned(FSkinnedPool& Pool, uint64& Offset, uint64 Size);
	void           ProcessPendingFrees();
	uint32         GetIndexSrv(const FStaticMesh& Mesh);
	void           PrepareSkinned(const FRayTracingSceneOptions& Options, const FResourceManager& Resources,
	                              const std::function<uint64(uint64)>& AllocScratch);
	uint32         RegisterMaterial(const FMaterial* Material, const FResourceManager& Resources, bool bGraphMaterials);
	bool           EnsureBuffer(ComPtr<ID3D12Resource>& Buffer, uint64& Capacity, uint64 Size, D3D12_RESOURCE_STATES State, const wchar_t* Name);
	ComPtr<ID3D12Resource> CreateBuffer(uint64 Size, D3D12_HEAP_TYPE Heap, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES State, const wchar_t* Name) const;
	FD3D12DescriptorHandle CreateRawSrv(ID3D12Resource* Resource, uint64 SizeInBytes);
	void           ReleaseStatic(FStaticBlas& Entry);
	void           ReleaseSkinnedPools();
	bool           CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);

	FD3D12RHI*     Rhi     = nullptr;
	FShaderLibrary* Library = nullptr;
	ID3D12Device5* Device5 = nullptr;
	FD3D12RootSignature SkinningRoot;     // b0 상수 2개, t0 기본 정점, t1 스킨 스트림, t15 팔레트, u0 출력 (루트 기술자)
	FD3D12PipelineState SkinningPipeline;

	std::unordered_map<uint64, std::unique_ptr<FStaticBlas>>  StaticCache;  // HashBlasKey → 항목 (충돌은 Key 비교로 확인)
	std::unordered_map<uint64, std::unique_ptr<FSkinnedPrimitive>> SkinnedPrimitives; // 엔티티 Id → 프리미티브
	std::unordered_map<uint64, std::unique_ptr<FSkinnedGroup>>     SkinnedGroups;     // 묶음 키 → 모델 BLAS
	std::unordered_map<ID3D12Resource*, FIndexSrv>                  IndexSrvs;         // 메시 인덱스 버퍼 → SRV
	FSkinnedPool                                                    SkinVertexPool;    // 월드 공간 스킨 정점 (FVertex), 그래프 밖 상태 = SkinVertexState
	D3D12_RESOURCE_STATES                                           SkinVertexState = D3D12_RESOURCE_STATE_COMMON;
	FD3D12DescriptorHandle                                          SkinVertexSrv;     // 풀 전체 raw SRV
	FSkinnedPool                                                    SkinBlasPool;      // 스킨 모델 BLAS
	std::vector<FPendingRangeFree>                                  PendingFrees;
	bool                                                            bScratchWrapped   = false; // Prepare 안 스크래치 할당이 처음으로 돌아감 (다음 빌드 op에 배리어)
	std::unordered_map<const FTerrainData*, std::unique_ptr<FTerrainBlas>> TerrainCache; // 지형 데이터 → 타일
	std::vector<std::unique_ptr<FSlot>>                        Slots;

	// 이번 프레임
	std::vector<D3D12_RAYTRACING_INSTANCE_DESC> TlasDescs;
	std::vector<FRayTracingInstanceGpu>         InstanceInfos;
	std::vector<FRayTracingMaterialGpu>         MaterialInfos;
	std::unordered_map<const FMaterial*, uint32> MaterialIndices;
	std::vector<FVector4>                       GraphParams;       // 그래프 머티리얼 머리 + 파라미터 (이번 프레임)
	std::vector<std::pair<uint32, std::shared_ptr<const FMaterialShader>>> GraphMaterialShaders; // (머티리얼 표 번호, 셰이더) — 슬롯은 Prepare 끝에 해시 순으로
	FRayTracingGraphVariant                     GraphVariant;
	D3D12_GPU_VIRTUAL_ADDRESS                   GraphParamAddress = 0;
	void                                        BuildGraphVariant(const FRayTracingSceneOptions& Options);
	std::vector<FBuildOp>                       BuildOps;
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> BuildGeometries;
	std::vector<FFrameSkinItem>                 FrameSkinItems;
	std::unordered_map<uint64, uint32>          FrameGroupOrder;    // 묶음 키 → 첫 등장 순서 (이번 프레임)
	std::vector<FSkinOp>                        SkinOps;
	std::vector<FCompactOp>                     CompactOps;
	std::vector<ID3D12Resource*>                FrameWrittenBlas;   // 이번 프레임 쓰인 BLAS (TLAS 빌드가 읽기 선언)
	bool                                        bFrameSkinned      = false; // 이번 프레임 TLAS에 스킨 지오메트리가 있다 (추적 패스가 정점 풀을 읽는다)
	bool                                        bFrameSkinnedBuild = false; // 이번 프레임 스킨 BLAS 빌드/갱신이 있다
	FRGResourceRef                              FrameSkinVertexRef; // 정점 풀의 그래프 참조 (AddBuildPasses → DeclareTraceReads)
	uint32                                      FrameSlot      = 0;
	uint64                                      FrameNumber    = 0;
	uint64                                      TlasScratchOffset = 0;
	uint64                                      TlasScratchSize   = 0;
	D3D12_GPU_VIRTUAL_ADDRESS                   TlasDescAddress   = 0;
	D3D12_GPU_VIRTUAL_ADDRESS                   TlasAddress       = 0;
	D3D12_GPU_VIRTUAL_ADDRESS                   InstanceBufferAddress = 0;
	D3D12_GPU_VIRTUAL_ADDRESS                   MaterialBufferAddress = 0;
	D3D12_GPU_VIRTUAL_ADDRESS                   PaletteAddress    = 0;
	bool                                        bPrepared         = false;
	FRayTracingSceneStats                       Stats;
};

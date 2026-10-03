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

#include <memory>
#include <unordered_map>
#include <vector>

class FD3D12RHI;
class FMeshInstanceList;
class FResourceManager;
class FShaderLibrary;
class FStaticMesh;
struct FMaterial;

// ---- GPU 구조 (RayTracingCommon.hlsli와 1:1)

// TLAS 인스턴스 하나의 기하/머티리얼 정보 (StructuredBuffer, 번호 = TLAS InstanceID). 정점/인덱스는 바인드리스 힙 칸 (ByteAddressBuffer)
struct FRayTracingInstanceGpu
{
	uint32 VertexBuffer = 0; // 힙 칸: FVertex 64B 정점 (스킨 = 이번 프레임 월드 공간 스키닝 결과)
	uint32 IndexBuffer  = 0; // 힙 칸: uint32 인덱스
	uint32 FirstIndex   = 0; // LOD 시작 (인덱스 단위) — 삼각형 i = 인덱스 [FirstIndex + 3i, +3)
	uint32 Material     = 0; // 머티리얼 표 번호
	uint32 Flags        = 0; // RayTracingMath::InstanceInfo*
	uint32 Padding[3]   = {};
};
static_assert(sizeof(FRayTracingInstanceGpu) == 32);

// 히트 머티리얼 (고정 PBR 상수 + 텍스처 테이블 힙 칸 — MaterialDefault.hlsli를 E_MATERIAL_CUSTOM_RESOURCES로 바인드리스 평가)
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
	uint32   Padding[2]        = {};

	static constexpr uint32 MaterialFlagGraph  = 1u << 0; // 그래프 머티리얼 → 고정 PBR 근사(회색) — 그래프 히트 셰이더는 후속(상태 객체)
	static constexpr uint32 MaterialFlagMasked = 1u << 1;
};
static_assert(sizeof(FRayTracingMaterialGpu) == 64);

struct FRayTracingSceneOptions
{
	FVector3 CameraPosition;
	bool     bSkinned             = true;     // 스킨 메시 (계산 셰이더 스키닝 + BLAS 갱신)
	bool     bFoliage             = true;     // 폴리지 인스턴스
	float    SkinnedMaxDistance   = 5000.0f;  // cm, 카메라에서 이보다 먼 스킨 메시는 TLAS에서 뺀다 (갱신 비용 상한)
	uint32   MaxBuildsPerFrame    = 32;       // 새 BLAS 빌드 수 상한 (끊김 방지 — 나머지는 다음 프레임, 그동안 TLAS에 없음)
	uint64   MaxBuildTriangles    = 2000000;  // 프레임당 새 BLAS 삼각형 상한
	bool     bCompaction          = true;     // 정적 BLAS 압축 (빌드 → 몇 프레임 뒤 크기 읽기 → 복사)
};

struct FRayTracingSceneStats
{
	uint32 StaticBlas        = 0; // 캐시의 정적 BLAS 수
	uint32 SkinnedBlas       = 0;
	uint64 BlasBytes         = 0; // 정적 + 스킨 BLAS 버퍼
	uint64 SkinnedVertexBytes = 0; // 스키닝 결과 정점 버퍼
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
//   스킨 메시: 엔티티별 월드 공간 정점 버퍼(계산 셰이더 RayTracingSkinning.hlsl — 프레임 팔레트 t15) + ALLOW_UPDATE BLAS를 프레임마다 갱신(refit).
//     카메라에서 SkinnedMaxDistance 안 + 팔레트 가시성 컬링을 통과한 것만 (FMeshInstanceList가 이미 뺀다).
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
	void Prepare(const FMeshInstanceList& Instances, const FResourceManager& Resources, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
	             const FRayTracingSceneOptions& Options);
	// 그래프 패스 등록. 반환 = TLAS 참조 (추적 패스가 Read(…, AccelStructRead))
	FRGResourceRef AddBuildPasses(FRenderGraph& Graph, int32 Timer);
	// 추적 패스가 읽는 리소스 선언: TLAS + 이번 프레임 스킨 정점 버퍼 (바인드리스로 읽음)
	void DeclareTraceReads(FRenderGraph::FPassBuilder& Pass, FRGResourceRef Tlas) const;

	D3D12_GPU_VIRTUAL_ADDRESS    GetTlasAddress() const { return TlasAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS    GetInstanceBuffer() const { return InstanceBufferAddress; }
	D3D12_GPU_VIRTUAL_ADDRESS    GetMaterialBuffer() const { return MaterialBufferAddress; }
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
	};
	struct FSkinnedBlas
	{
		FEntity                   Entity;
		FMeshHandle               MeshHandle;
		const FStaticMesh*        Mesh = nullptr;
		ComPtr<ID3D12Resource>    Vertices; // 월드 공간 스키닝 결과 (FVertex)
		D3D12_RESOURCE_STATES     VertexState = D3D12_RESOURCE_STATE_COMMON;
		ComPtr<ID3D12Resource>    Blas;
		uint64                    BlasSize    = 0;
		uint64                    ScratchSize = 0; // 빌드/갱신 중 큰 값
		uint32                    VertexCount = 0;
		uint32                    IndexCount  = 0;
		bool                      bBuilt      = false;
		uint64                    LastUsedFrame = 0;
		FD3D12DescriptorHandle    VertexSrv;
		FD3D12DescriptorHandle    IndexSrv;
		// 이번 프레임
		bool                      bActive     = false;
		uint32                    BoneOffset  = 0;
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
		ID3D12Resource*                                     Dest   = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS                           Source = 0; // 갱신이면 = Dest 주소
		D3D12_RAYTRACING_GEOMETRY_DESC                      Geometry{};
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
		uint64                                              ScratchOffset = 0;
		int32                                               PostbuildIndex = -1; // 압축 크기 기록 칸 (-1 = 없음)
	};
	struct FSkinOp
	{
		ID3D12Resource*           Output = nullptr;
		D3D12_RESOURCE_STATES*    State  = nullptr;
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
	void           EvictUnused();
	FStaticBlas*   FindOrCreateStatic(const FStaticMesh& Mesh, FMeshHandle Handle, uint32 Lod);
	FSkinnedBlas*  FindOrCreateSkinned(FEntity Entity, const FStaticMesh& Mesh, FMeshHandle Handle);
	uint32         RegisterMaterial(const FMaterial* Material, const FResourceManager& Resources);
	bool           EnsureBuffer(ComPtr<ID3D12Resource>& Buffer, uint64& Capacity, uint64 Size, D3D12_RESOURCE_STATES State, const wchar_t* Name);
	ComPtr<ID3D12Resource> CreateBuffer(uint64 Size, D3D12_HEAP_TYPE Heap, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES State, const wchar_t* Name) const;
	FD3D12DescriptorHandle CreateRawSrv(ID3D12Resource* Resource, uint64 SizeInBytes);
	void           ReleaseStatic(FStaticBlas& Entry);
	void           ReleaseSkinned(FSkinnedBlas& Entry);
	bool           CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);

	FD3D12RHI*     Rhi     = nullptr;
	FShaderLibrary* Library = nullptr;
	ID3D12Device5* Device5 = nullptr;
	FD3D12RootSignature SkinningRoot;     // b0 상수 2개, t0 기본 정점, t1 스킨 스트림, t15 팔레트, u0 출력 (루트 기술자)
	FD3D12PipelineState SkinningPipeline;

	std::unordered_map<uint64, std::unique_ptr<FStaticBlas>>  StaticCache;  // HashBlasKey → 항목 (충돌은 Key 비교로 확인)
	std::unordered_map<uint64, std::unique_ptr<FSkinnedBlas>> SkinnedCache; // 엔티티 Id → 항목
	std::vector<std::unique_ptr<FSlot>>                        Slots;

	// 이번 프레임
	std::vector<D3D12_RAYTRACING_INSTANCE_DESC> TlasDescs;
	std::vector<FRayTracingInstanceGpu>         InstanceInfos;
	std::vector<FRayTracingMaterialGpu>         MaterialInfos;
	std::unordered_map<const FMaterial*, uint32> MaterialIndices;
	std::vector<FBuildOp>                       BuildOps;
	std::vector<FSkinOp>                        SkinOps;
	std::vector<FCompactOp>                     CompactOps;
	std::vector<ID3D12Resource*>                FrameWrittenBlas;   // 이번 프레임 쓰인 BLAS (TLAS 빌드가 읽기 선언)
	std::vector<FSkinnedBlas*>                  FrameSkinned;       // 이번 프레임 활성 스킨 항목
	std::vector<FRGResourceRef>                 FrameSkinVertexRefs; // 그 정점 버퍼의 그래프 참조 (AddBuildPasses → DeclareTraceReads)
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

#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/RenderGraph/RenderGraph.h"

#include <utility>
#include <vector>

class FD3D12DynamicUploadBuffer;
class FD3D12RHI;
class FMeshInstanceList;
class FShaderLibrary;

// 스킨 캐시 배치 (순수 로직 — SkinCacheTests): 이번 프레임 스킨 인스턴스(입력 순서)를 메시별로 묶어 캐시 정점 영역에 연속 배치한다.
namespace SkinCacheMath
{
	// 버퍼 = 영역 4개, 용량 C(정점) 기준 (SkinnedMesh.hlsli E_SKIN_CACHE — 래스터가 읽는 것은 촘촘한 SoA, 쓰기 대역폭이 비용의 대부분):
	//   위치 [0, 16C)          float4 (월드 xyz, w = 가중치 합) — 그림자/사전/메인
	//   법선·탄젠트 [16C, 48C)  float4 (법선 xyz, 탄젠트 w) + float4 (탄젠트 xyz, 0) — 메인/그래프 그림자
	//   이전 위치 [48C, 64C)    float4 (이전 프레임 월드 xyz, w) — 움직임 벡터
	//   RT 정점 [64C, 64C + 64R) FVertex 64B (UV/색 포함, 레이 트레이싱 정점 풀로 그대로 복사) — RT 용량 R은 RT 대상 인스턴스만 따로 센다
	inline constexpr uint64 PositionBytes  = 16;
	inline constexpr uint64 TangentBytes   = 32;
	inline constexpr uint64 PrevBytes      = 16;
	inline constexpr uint64 RasterBytesPerVertex = PositionBytes + TangentBytes + PrevBytes; // 64
	inline constexpr uint64 RtVertexBytes  = 64; // sizeof(FVertex)
	inline constexpr uint64 MaxBytesPerVertex = RasterBytesPerVertex + RtVertexBytes;
	inline constexpr uint32 GroupSize          = 64;     // SkinCache.hlsl numthreads X
	inline constexpr uint32 MaxDispatchItems   = 65535;  // 디스패치 Y 상한 (넘으면 같은 메시를 여러 디스패치로)
	inline constexpr uint64 CapacityGranularity = 16384; // 용량 올림 단위 (정점)
	// 바이트 주소 버퍼 오프셋은 uint32 — 이전 영역 끝(용량 × 80바이트)이 4GB를 넘지 않게
	inline constexpr uint64 MaxCapacity = (0xFFFFFFFFull / MaxBytesPerVertex) / CapacityGranularity * CapacityGranularity;

	struct FItemInput
	{
		const void* MeshKey     = nullptr; // 같은 키 = 같은 디스패치 (메시 + 스키닝할 정점 목록)
		uint32      VertexCount = 0;       // 캐시 자리 크기 = 메시 정점 수 (정점 번호 그대로 자리)
		uint32      ThreadCount = 0;       // 스키닝할 정점 수 (LOD 정점 목록 크기, 항등이면 VertexCount)
		uint32      Variant     = 0;       // 같은 키라도 변형(예: RT 속성 쓰기)이 다르면 다른 디스패치
	};
	// 같은 메시 인스턴스 묶음 하나 = 디스패치 하나 (X = 정점 그룹, Y = 항목)
	struct FDispatch
	{
		uint32 FirstItem   = 0; // ItemOrder 안 첫 항목 (= GPU 항목 표 위치)
		uint32 ItemCount   = 0; // MaxDispatchItems 이하
		uint32 VertexCount = 0;
		uint32 ThreadCount = 0; // 디스패치 X 스레드 (정점 목록 크기)
		uint32 Variant     = 0;
	};
	struct FLayout
	{
		std::vector<uint32>    ItemOrder;   // 디스패치 순서의 입력 번호 (같은 메시가 연속, 메시는 첫 등장 순서, 메시 안은 입력 순서)
		std::vector<uint32>    FirstVertex; // 입력 번호별 영역 안 첫 정점 (정점 0개 입력은 0 — 디스패치 없음)
		std::vector<FDispatch> Dispatches;
		uint64                 TotalVertices = 0;
	};
	// 결정적: 결과는 입력 순서만으로 정해진다 (포인터 값의 크기 비교·해시 순회 없음)
	void BuildLayout(const std::vector<FItemInput>& Inputs, FLayout& Out);
	// 메시 LOD별 스키닝할 정점 목록: OutLists[L] = LOD L..마지막 인덱스 범위가 쓰는 정점 번호 (오름차순, 중복 없음).
	// 모든 정점을 쓰면 빈 목록 (항등). LodRanges = (인덱스 시작, 개수)
	void BuildLodVertexLists(uint32 VertexCount, const std::vector<uint32>& Indices, const std::vector<std::pair<uint32, uint32>>& LodRanges,
	                         std::vector<std::vector<uint32>>& OutLists);
	// 필요한 용량 (정점): Required가 Current 이하면 Current 유지(줄이지 않음 — 다시 만들면 버퍼 교체), 넘으면 1.5배와 Required 중 큰 값을 단위로 올림.
	// MaxCapacity를 넘으면 0 (할 수 없음)
	uint64 ComputeCapacity(uint64 Current, uint64 Required);
	// 영역 시작 바이트 (위 머리 주석 순서)
	inline uint64 GetTangentOffset(uint64 Capacity) { return Capacity * PositionBytes; }
	inline uint64 GetPrevOffset(uint64 Capacity) { return Capacity * (PositionBytes + TangentBytes); }
	inline uint64 GetRtOffset(uint64 Capacity) { return Capacity * RasterBytesPerVertex; }
	inline uint64 GetBufferBytes(uint64 Capacity, uint64 RtCapacity) { return Capacity * RasterBytesPerVertex + RtCapacity * RtVertexBytes; }
} // namespace SkinCacheMath

// 레이 트레이싱 스킨 BLAS 공유 (FRayTracingScene): 켜져 있으면 LOD0이고 RT 스킨 거리 안인 인스턴스는 UV/색까지 써서
// (RT 정점 영역에 완전한 FVertex) RT가 다시 스키닝하지 않고 복사하게 한다. 그 밖 인스턴스는 래스터가 읽는 위치/법선/탄젠트만 쓴다(쓰기 대역폭)
struct FSkinCacheRtOptions
{
	bool     bEnabled    = false;
	FVector3 CameraPosition;
	float    MaxDistance = 0.0f; // cm (r.RayTracing.Skinned.MaxDistance)
};

// 메시 패스가 스킨 정점을 읽는 곳 (루트 SRV t15): 스킨 캐시면 캐시 버퍼 + 그래프 참조(읽기 선언), 아니면 프레임 팔레트(업로드 힙 — 선언 없음)
struct FSkinDrawSource
{
	D3D12_GPU_VIRTUAL_ADDRESS Address = 0;
	FRGResourceRef            CacheRef;

	void DeclareRead(FRenderGraph::FPassBuilder& Pass) const
	{
		if (CacheRef.IsValid())
		{
			Pass.Read(CacheRef, ERGAccess::SrvNonPixel);
		}
	}
};

// 스킨 캐시 (r.SkinCache): 프레임마다 보이는 스킨 인스턴스(팔레트 가시성을 통과해 메시 인스턴스 목록에 든 것)를 계산 셰이더로 한 번 스키닝해
// 월드 공간 정점(위치·법선·탄젠트 + 이전 프레임 위치, 촘촘한 SoA 영역)을 풀 버퍼 하나에 쓴다. 메시 패스(깊이 사전/메인/반투명/방향광·로컬 그림자)의 스킨 변형은
// 디파인 E_SKIN_CACHE 정점 셰이더로 이 버퍼를 SV_VertexID + 인스턴스 SkinCacheVertex로 읽는다 (패스마다 다시 스키닝하지 않음).
//   - 묶음/인스턴싱은 그대로 (같은 메시 인스턴스는 인스턴스 데이터의 캐시 위치만 다르다)
//   - LOD는 정점 버퍼를 공유한다: 캐시 자리는 메시 전체 정점(정점 번호 그대로)이지만 스키닝은 인스턴스 LOD 이상이 쓰는 정점만
//     (FStaticMesh::GetLodVertexList — 그림자 LOD 바이어스는 더 거친 LOD만 쓰므로 덮인다). 나머지 칸은 쓰지 않고 아무도 읽지 않는다
//   - 이전 위치 = 이전 프레임 팔레트(PrevBoneOffset)로 같은 정점을 변환 (이력이 없으면 현재 위치) — 움직임 벡터 식은 팔레트 경로와 같다
//   - 레이 트레이싱 스킨 BLAS(FRayTracingScene, LOD0)는 LOD0·RT 거리 안 인스턴스(bSkinCacheLod0)면 다시 스키닝하지 않고
//     이 버퍼의 RT 정점 영역(FVertex 그대로 — 그 인스턴스만 씀)을 자기 정점 풀로 복사한다 (아니면 자기 계산 스키닝)
//   - 배치는 Prepare마다 새로 (프레임마다 모두 다시 쓰므로 엔티티별 고정 자리가 필요 없다). 한 프레임에 여러 번 그려도(반사 캡처) 그래프 순서대로
// 호출: 메시 인스턴스 Gather·LOD 선택 뒤·Upload 전 Prepare(인스턴스의 SkinCacheVertex/SkinCacheCapacity를 채움) → 첫 메시 패스 등록 전 AddPass
class FSkinCache
{
public:
	~FSkinCache();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 스킨 인스턴스에 캐시 자리를 배정한다 (Gather 범위만 — AddExternal 인스턴스는 스킨이 아니다). 스킨 인스턴스가 없거나 버퍼를 못 만들면 false
	// (그때 스킨 인스턴스의 캐시 자리는 0 — 호출자는 이번 프레임 스킨 캐시 경로를 쓸 수 없다)
	bool Prepare(FMeshInstanceList& Instances, FD3D12DynamicUploadBuffer& DynamicBuffer, const FSkinCacheRtOptions& RtOptions = {});
	// 스키닝 계산 패스 등록 (Prepare가 true였을 때). Palette = 프레임 팔레트(t15). 반환 = 캐시 버퍼 참조 (읽는 패스가 선언)
	FRGResourceRef AddPass(FRenderGraph& Graph, D3D12_GPU_VIRTUAL_ADDRESS Palette, int32 Timer);

	ID3D12Resource*           GetBuffer() const { return Buffer.Get(); }
	D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const { return Buffer ? Buffer->GetGPUVirtualAddress() : 0; }
	uint64                    GetCapacity() const { return Capacity; }       // 정점
	uint64                    GetFrameVertices() const { return FrameVertices; } // 이번 Prepare가 스키닝하는 정점 수 (LOD 목록 합)
	uint32                    GetFrameDispatches() const { return static_cast<uint32>(FrameDispatches.size()); }
	uint64                    GetGpuBytes() const { return SkinCacheMath::GetBufferBytes(Capacity, RtCapacity); }

private:
	struct FFrameDispatch
	{
		D3D12_GPU_VIRTUAL_ADDRESS BaseVertices = 0;
		D3D12_GPU_VIRTUAL_ADDRESS SkinVertices = 0;
		D3D12_GPU_VIRTUAL_ADDRESS VertexList   = 0; // 스키닝할 정점 번호 목록 (0 = 항등)
		uint32                    ThreadCount  = 0;
		uint32                    FirstItem    = 0;
		uint32                    ItemCount    = 0;
		uint32                    bAttributes  = 0; // 1 = UV/색도 쓴다 (RT 복사용 완전한 FVertex)
	};
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	bool EnsureCapacity(uint64 Required, uint64 RtRequired);

	FD3D12RHI*                  Rhi     = nullptr;
	FShaderLibrary*             Library = nullptr;
	FD3D12RootSignature         RootSignature; // b0 상수 3개, t0 기본 정점, t1 스킨 스트림, t2 항목 표, t15 팔레트, u0 캐시, t3 정점 목록
	FD3D12PipelineState         Pipeline;
	ComPtr<ID3D12Resource>      Buffer;
	D3D12_RESOURCE_STATES       BufferState = D3D12_RESOURCE_STATE_COMMON; // 그래프 밖 상태 (ImportTracked)
	uint64                      Capacity    = 0;
	uint64                      RtCapacity  = 0; // RT 정점 영역 용량 (정점, RT 대상 인스턴스만)

	// 이번 Prepare
	SkinCacheMath::FLayout             Layout;
	std::vector<SkinCacheMath::FItemInput> LayoutInputs; // Layout을 만든 입력 (같으면 다시 만들지 않는다)
	std::vector<SkinCacheMath::FItemInput> Inputs;
	std::vector<uint32>                InputInstances; // 입력 번호 → 인스턴스 번호
	struct FInstanceInput
	{
		SkinCacheMath::FItemInput  Input;
		std::pair<uint32, uint32>  Bones;
		bool                       bSkinned = false;
	};
	std::vector<FInstanceInput>        PerInstance;    // 인스턴스 번호별 (병렬 수집)
	std::vector<std::pair<uint32, uint32>> InputBones;  // 입력 번호 → (BoneOffset, PrevBoneOffset)
	std::vector<uint32>                InputRtFirst;   // 입력 번호 → RT 정점 영역 첫 정점
	std::vector<FFrameDispatch>        FrameDispatches;
	D3D12_GPU_VIRTUAL_ADDRESS          ItemTable     = 0;
	uint64                             FrameVertices = 0;
	bool                               bPrepared     = false;
};

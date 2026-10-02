#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"
#include "Renderer/RenderGraph/RenderGraphCompiler.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class FD3D12RHI;
class FD3D12RenderTarget;
class FRenderGraph;

// ---- 렌더 그래프 (Phase 47). 프레임마다 FSceneRenderer::Render가 하나 만들어 패스를 등록하고 Compile → Execute한다.
//
// 새 패스 추가 방법 (Phase 48~ 규칙):
//   1. 리소스: 렌더러가 소유한 것(씬 타깃·이력·볼륨·그림자 맵 등)은 Import(시작 상태 = 평소 상태, 끝 상태 = 평소 상태 또는 None),
//      프레임 안에서만 쓰는 중간 텍스처는 CreateTexture(크기·형식 키로 풀링 — 프레임마다 다시 만들지 않는다).
//   2. AddPass(이름, 큐).Read/Write(리소스, 접근, 범위)로 이 패스가 GPU에서 읽고 쓰는 모든 리소스(서브리소스)를 선언하고
//      Execute 람다에서 명령만 기록한다. **람다 안에서 ResourceBarrier를 부르지 않는다** — 전이·UAV 배리어는 그래프가 패스 앞에 묶어서 한다.
//      렌더 타깃은 FD3D12RenderTarget::Bind(전이 없는 바인딩/클리어)를 쓴다 (Begin/End는 그래프 밖 코드용).
//   3. CPU 준비(수집·정렬·상수 업로드 계산)는 등록 시점에, 람다는 기록만. 람다는 비동기 계산 포크 때문에 등록 순서와 다른 CPU 순서로
//      불릴 수 있으므로 다른 패스 람다가 만든 CPU 데이터에 기대지 않는다.
//   4. 외부(가져온) 리소스에 쓰지 않고 결과를 아무도 읽지 않는 패스는 제거된다. 통계·리드백처럼 부수 효과만 있는 패스는 NeverCull().
//   5. 계산 셰이더만 쓰는 패스는 ERGQueue::AsyncCompute로 등록할 수 있다 (계산 큐 합법 상태 SrvNonPixel/Uav/Copy*만 선언,
//      람다는 Context.CommandList에 기록 — Rhi->GetCommandList()를 쓰지 않는다). r.RenderGraph.AsyncCompute 0이면 그래픽스 큐에서 순서대로.
//   6. GPU 구간 측정은 Timer(ERenderTimer 번호) — 같은 번호가 이어지는 패스는 한 구간으로 잰다.
struct FRGResourceRef
{
	static constexpr uint32 InvalidId = ~0u;
	uint32                  Id        = InvalidId;
	bool                    IsValid() const { return Id != InvalidId; }
};

// 그래프 내부 텍스처 설명 (풀 키 — 모든 필드가 같아야 재사용)
struct FRGTextureDesc
{
	uint32      Width     = 1;
	uint32      Height    = 1;
	uint16      DepthOrArraySize = 1;
	uint16      MipCount  = 1;
	DXGI_FORMAT Format    = DXGI_FORMAT_UNKNOWN;
	bool        b3D       = false;
	bool        bRenderTarget    = false; // RTV (밉 0) + 최적 클리어 값
	bool        bUnorderedAccess = false; // UAV (밉별)
	float       ClearColor[4]    = {};

	bool operator==(const FRGTextureDesc& Other) const;
	static FRGTextureDesc MakeRenderTarget(uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat)
	{
		FRGTextureDesc Desc;
		Desc.Width         = InWidth;
		Desc.Height        = InHeight;
		Desc.Format        = InFormat;
		Desc.bRenderTarget = true;
		return Desc;
	}
};

// 풀 텍스처 (리소스 + 뷰). 상태는 서브리소스별로 풀이 기억한다 (다음 그래프의 시작 상태)
struct FRGPooledTexture
{
	ComPtr<ID3D12Resource>              Resource;
	FRGTextureDesc                      Desc;
	FD3D12DescriptorHandle              Srv;  // 셰이더 가시 힙, 전체 밉
	std::vector<FD3D12DescriptorHandle> Uavs; // 밉별 (bUnorderedAccess)
	FD3D12DescriptorHeap                RtvHeap;
	std::vector<ERGAccess>              States;
	uint64                              LastUsedFrame = 0;
	bool                                bInUse        = false;

	D3D12_CPU_DESCRIPTOR_HANDLE GetRtv() const { return RtvHeap.GetCpuHandle(0); }
	uint64                      GetSizeBytes() const;
};

// 그래프 내부 텍스처 풀 (렌더러마다 하나). 같은 프레임의 여러 그래프가 차례로 같은 항목을 재사용한다 (한 큐 순서 + 그래프 끝 조인)
class FRGResourcePool
{
public:
	~FRGResourcePool();
	void Init(FD3D12RHI& InRhi) { Rhi = &InRhi; }
	void Shutdown();

	FRGPooledTexture* Acquire(const FRGTextureDesc& Desc, const char* Name);
	void              ReleaseAll(); // 그래프 실행 끝: 모두 사용 가능
	void              Trim(uint64 FrameNumber, uint64 UnusedFrames = 60); // 오래 안 쓴 항목 지연 해제

	uint32 GetTextureCount() const { return static_cast<uint32>(Textures.size()); }
	uint64 GetTotalBytes() const;

private:
	bool CreateTexture(FRGPooledTexture& Texture, const char* Name);

	FD3D12RHI*                                     Rhi = nullptr;
	std::vector<std::unique_ptr<FRGPooledTexture>> Textures;
};

struct FRGContext
{
	ID3D12GraphicsCommandList* CommandList   = nullptr; // 그래픽스 패스 = Rhi 프레임 목록, 비동기 계산 패스 = 계산 큐 목록
	bool                       bAsyncCompute = false;
	FRenderGraph*              Graph         = nullptr;

	// 같은 패스 안에서 같은 UAV에 연달아 쓰는 디스패치 사이 (상태는 바뀌지 않는다 — 상태 전이는 그래프만 한다)
	void UavBarrier(FRGResourceRef Resource) const;
};

using FRGExecuteFn = std::function<void(FRGContext&)>;

// 그래프 실행 통계 (마지막 실행, 통계 창·덤프용)
struct FRGStats
{
	uint32 Passes          = 0;
	uint32 CulledPasses    = 0;
	uint32 AsyncPasses     = 0;
	uint32 AsyncBatches    = 0;
	uint32 Transitions     = 0;
	uint32 UavBarriers     = 0;
	uint32 BarrierBatches  = 0;
	uint32 Resources       = 0;
	uint32 ImportedResources = 0;
	uint32 PooledTextures  = 0;
	uint64 PooledBytes     = 0;
	float  CompileMs       = 0.0f;
};

class FRenderGraph
{
public:
	// 등록 빌더 (AddPass가 돌려줌). 접근 선언은 Execute 전에
	class FPassBuilder
	{
	public:
		FPassBuilder& Read(FRGResourceRef Resource, ERGAccess Access, FRGSubresourceRange Range = FRGSubresourceRange::All());
		// bOverwrite: 이전 내용을 읽지 않고 전부 덮어씀 (컬링 의존을 끊는다 — 전체 화면 패스 출력 등)
		FPassBuilder& Write(FRGResourceRef Resource, ERGAccess Access, FRGSubresourceRange Range = FRGSubresourceRange::All(), bool bOverwrite = false);
		FPassBuilder& NeverCull();
		FPassBuilder& Timer(int32 TimerId);
		FPassBuilder& Execute(FRGExecuteFn Function);

	private:
		friend class FRenderGraph;
		FPassBuilder(FRenderGraph& InGraph, uint32 InIndex) : Graph(&InGraph), Index(InIndex) {}
		FRenderGraph* Graph = nullptr;
		uint32        Index = 0;
	};

	FRenderGraph(FD3D12RHI& InRhi, FRGResourcePool& InPool, const char* InName);
	~FRenderGraph();
	FRenderGraph(const FRenderGraph&)            = delete;
	FRenderGraph& operator=(const FRenderGraph&) = delete;

	// 외부 리소스. FinalState None = 그래프가 끝낸 상태 그대로 (GetFinalState로 받아 외부 추적 상태를 갱신)
	FRGResourceRef Import(const char* Name, ID3D12Resource* Resource, ERGAccess InitialState, ERGAccess FinalState, uint32 MipCount = 1,
	                      uint32 ArraySize = 1);
	FRGResourceRef ImportPerSubresource(const char* Name, ID3D12Resource* Resource, const std::vector<ERGAccess>& InitialStates, ERGAccess FinalState,
	                                    uint32 MipCount, uint32 ArraySize = 1);
	// 소유자가 D3D12 상태 하나로 추적하는 리소스: 시작 = *TrackedState, 끝 = 그래프가 남긴 상태(서브리소스는 0번 상태로 맞춤)를
	// Execute 끝에 *TrackedState에 다시 쓴다 (포인터는 Execute까지 유효해야 한다 — 렌더러 멤버)
	FRGResourceRef ImportTracked(const char* Name, ID3D12Resource* Resource, D3D12_RESOURCE_STATES* TrackedState, uint32 MipCount = 1,
	                             uint32 ArraySize = 1);
	// 렌더 타깃 색 (평소 SrvPixel) / 깊이 (평소 DepthWrite)
	FRGResourceRef ImportColor(const char* Name, const FD3D12RenderTarget& Target);
	FRGResourceRef ImportDepth(const char* Name, const FD3D12RenderTarget& Target);
	// 같은 ID3D12Resource를 이미 가져왔으면 그 참조 (없으면 무효)
	FRGResourceRef FindImported(ID3D12Resource* Resource) const;

	// 그래프 내부 텍스처 (풀에서 바로 얻는다 — 뷰는 등록 시점부터 유효)
	FRGResourceRef           CreateTexture(const char* Name, const FRGTextureDesc& Desc);
	const FRGPooledTexture*  GetTexture(FRGResourceRef Resource) const;
	ID3D12Resource*          GetResource(FRGResourceRef Resource) const;

	FPassBuilder AddPass(const char* Name, ERGQueue Queue = ERGQueue::Graphics);

	// 측정 훅 (같은 Timer 번호가 이어지는 그래픽스 패스는 한 구간). bCompute = 계산 큐 목록
	std::function<void(int32 Timer, ID3D12GraphicsCommandList* List, bool bCompute)> OnTimerBegin;
	std::function<void(int32 Timer, ID3D12GraphicsCommandList* List, bool bCompute)> OnTimerEnd;
	// 마지막 비동기 묶음을 제출하기 직전 (계산 큐 타이머 쿼리 정리)
	std::function<void(ID3D12GraphicsCommandList* List)> OnLastComputeBatchEnd;

	void Compile(const FRGCompileOptions& Options);
	// 프레임 명령 목록(Rhi)에 기록 (비동기 묶음이면 중간 제출 + 계산 큐). ExternalList를 주면 그 목록에만 기록한다
	// (로딩 때 즉시 실행 목록 등 — 비동기 계산 없이 컴파일해야 한다)
	void Execute(ID3D12GraphicsCommandList* ExternalList = nullptr);

	const FRGCompileResult& GetCompileResult() const { return Compiled; }
	const FRGStats&         GetStats() const { return Stats; }
	ERGAccess               GetFinalState(FRGResourceRef Resource, uint32 Subresource = 0) const;
	// 덤프 (패스 순서·큐·제거된 패스·전이 수·포크/조인·리소스 수명)
	std::vector<std::string> Dump() const;

private:
	struct FResource
	{
		std::string       Name;
		ID3D12Resource*   Resource  = nullptr;
		FRGPooledTexture* Pooled    = nullptr;
		bool              bImported = false;
		uint32            MipCount  = 1;
		uint32            ArraySize = 1;
		std::vector<ERGAccess> InitialStates;
		ERGAccess         FinalState = ERGAccess::None;
		D3D12_RESOURCE_STATES* TrackedState = nullptr; // ImportTracked
	};
	struct FPass
	{
		std::string                   Name;
		ERGQueue                      Queue      = ERGQueue::Graphics;
		bool                          bNeverCull = false;
		int32                         Timer      = -1;
		std::vector<FRGCompileAccess> Accesses;
		FRGExecuteFn                  Function;
	};

	void RecordBarriers(ID3D12GraphicsCommandList* List, const std::vector<FRGBarrier>& Barriers);
	void RunPass(uint32 PassIndex, ID3D12GraphicsCommandList* List, bool bCompute, int32& OpenTimer);

	FD3D12RHI&             Rhi;
	FRGResourcePool&       Pool;
	std::string            Name;
	std::vector<FResource> Resources;
	std::vector<FPass>     Passes;
	FRGCompileResult       Compiled;
	FRGStats               Stats;
	bool                   bCompiled = false;
	bool                   bExecuted = false;
	std::vector<D3D12_RESOURCE_BARRIER> BarrierScratch;
};

namespace RenderGraphD3D12
{
	D3D12_RESOURCE_STATES ToD3D12(ERGAccess Access);
	ERGAccess             FromD3D12(D3D12_RESOURCE_STATES States); // 0 = Common
}

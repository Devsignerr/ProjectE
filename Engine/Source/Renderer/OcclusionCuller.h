#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShaderTypes.h"

#include <vector>

class FD3D12RenderTarget;
class FMeshInstanceList;
class FMeshPassBatches;
class FShaderLibrary;

// 오클루전 버퍼들의 그래프 참조
struct FOcclusionGraphRefs
{
	FRGResourceRef DrawArguments;
	FRGResourceRef Phase1Indices;
	FRGResourceRef Phase2Indices;
	FRGResourceRef Occluded;
	FRGResourceRef Hzb;
};

// HZB 오클루전 컬링 (메인 패스의 정적 메시 묶음만, GPU 계산 + ExecuteIndirect). 식은 Renderer/HzbMath.h
//   흐름 (FSceneRenderer::RenderSceneColor, 렌더 그래프 패스):
//     1) 1단계 컬링: 이전 프레임 HZB(+그 프레임 뷰-투영)로 인스턴스마다 판정 → 보이는 것만 1단계 목록/간접 인자에
//     2) 1단계 드로우: 묶음마다 ExecuteIndirect(인자 = 묶음 * 2), 정점 셰이더 인스턴스 목록(t14) = GetIndices(1)
//     3) HZB + 2단계 컬링: 지금까지의 깊이로 HZB(밉마다 패스)를 만들고, 1단계에서 가려진 것만 이번 프레임 HZB로 다시 판정 → 2단계 목록
//     4) 2단계 드로우 (인자 = 묶음 * 2 + 1)
//   깜빡임 대책: 이전 프레임 HZB만 쓰면 새로 드러난(이전 프레임엔 가려진) 물체가 한 프레임 늦게 나타나는데, 2단계가 이번 프레임
//   깊이로 다시 검사해 같은 프레임에 그린다 (보수적: 1단계 깊이는 최종 깊이보다 멀거나 같으므로 보이는 것을 버리지 않는다).
//   HZB는 1단계 깊이로 만든 것을 다음 프레임 1단계가 재사용한다 (프레임당 한 번). 크기가 바뀐 첫 프레임은 모두 보이는 것으로.
//   스킨 메시는 대상이 아니다 (1단계에 그대로 그린다). 그림자 패스는 카메라 HZB와 무관하므로 적용하지 않는다.
class FOcclusionCuller
{
public:
	~FOcclusionCuller();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 렌더 그래프 흐름 (FSceneRenderer):
	//   PreparePhase1(CPU: 항목·간접 인자 업로드, 버퍼/HZB 준비) → Import → AddPhase1Passes(인자 초기화 복사 + 1단계 컬링)
	//   → [1단계 드로우 패스: GetIndices(1) + 간접 인자] → AddHzbAndPhase2Passes(HZB 밉마다 계산 패스 + 2단계 컬링)
	//   → [2단계 드로우 패스: GetIndices(2)] → AddFinishPass(통계 리드백 복사, 부수 효과)
	// 드로우 패스는 DrawArguments를 IndirectArgs, 쓰는 단계 목록을 SrvNonPixel로 선언한다.
	void PreparePhase1(const FMeshInstanceList& Instances, const FMeshPassBatches& Batches, uint32 Width, uint32 Height);
	FOcclusionGraphRefs Import(FRenderGraph& Graph);
	void AddPhase1Passes(FRenderGraph& Graph, const FOcclusionGraphRefs& Refs, int32 Timer);
	// 깊이(1단계를 그린 씬 깊이)로 HZB → 2단계 컬링
	void AddHzbAndPhase2Passes(FRenderGraph& Graph, const FOcclusionGraphRefs& Refs, FRGResourceRef Depth, const FD3D12RenderTarget& SceneColor,
	                           const FMatrix4x4& ViewProjection, int32 Timer);
	void AddFinishPass(FRenderGraph& Graph, const FOcclusionGraphRefs& Refs);

	// 단계(1/2) 인스턴스 번호 목록 (NON_PIXEL_SHADER_RESOURCE, 정점 셰이더 t14)
	D3D12_GPU_VIRTUAL_ADDRESS GetIndices(uint32 Phase) const;
	// 묶음 하나의 단계 드로우 (메시 버퍼·루트 상수는 호출자가 바인딩)
	void DrawIndirect(ID3D12GraphicsCommandList* CommandList, uint32 Batch, uint32 Phase) const;

	// 통계 (GPU 인자 리드백 — 몇 프레임 늦은 값)
	uint32 GetTestedInstances() const { return StatTested; }
	uint32 GetPhase1Instances() const { return StatPhase1; }
	uint32 GetPhase2Instances() const { return StatPhase2; }
	uint64 GetDrawnTriangles() const { return StatTriangles; }

private:
	bool CreatePipelines(bool bForceRecompile, FD3D12PipelineState& OutFromDepth, FD3D12PipelineState& OutDownsample, FD3D12PipelineState& OutCull);
	void EnsureBuffers(uint32 SlotCount, uint32 ItemCount, uint32 BatchCount);
	void EnsureHzb(uint32 Width, uint32 Height);
	void ReleaseHzb();
	void ReadStats();
	// 컬링 디스패치 기록 (Phase 1/2)
	void RecordCull(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress, uint32 Phase) const;

	// PreparePhase1 결과 (패스 람다가 읽는다)
	FD3D12DynamicAllocation ArgumentsUpload;
	uint64                  ArgumentsBytes  = 0;
	D3D12_GPU_VIRTUAL_ADDRESS Phase1Constants = 0;

	struct FBuffer
	{
		ComPtr<ID3D12Resource> Resource;
		D3D12_RESOURCE_STATES  State    = D3D12_RESOURCE_STATE_COMMON;
		uint64                 Capacity = 0; // 바이트
	};
	void EnsureBuffer(FBuffer& Buffer, uint64 Bytes, const wchar_t* DebugName);

	FD3D12RHI*      Rhi           = nullptr;
	FShaderLibrary* ShaderLibrary = nullptr;

	FD3D12RootSignature            HzbRootSignature;  // b0 상수 4개, t0 표, u0 표, u1 표
	FD3D12RootSignature            CullRootSignature; // b1 CBV, t1 SRV, t2 표, u2~u5 UAV
	FD3D12PipelineState            HzbFromDepthPipeline;
	FD3D12PipelineState            HzbDownsamplePipeline;
	FD3D12PipelineState            CullPipeline;
	ComPtr<ID3D12CommandSignature> DrawSignature; // DRAW_INDEXED만

	FBuffer DrawArguments; // 묶음마다 단계 1/2 D3D12_DRAW_INDEXED_ARGUMENTS
	FBuffer Phase1Indices;
	FBuffer Phase2Indices;
	FBuffer Occluded;

	// HZB (R32_FLOAT, 밉 체인)
	ComPtr<ID3D12Resource>              Hzb;
	D3D12_RESOURCE_STATES               HzbState = D3D12_RESOURCE_STATE_COMMON;
	uint32                              HzbWidth  = 0; // 밉 0
	uint32                              HzbHeight = 0;
	uint32                              HzbMips   = 0;
	uint32                              HzbDepthWidth  = 0; // HZB를 만든 깊이 크기
	uint32                              HzbDepthHeight = 0;
	FD3D12DescriptorHandle              HzbSrv;           // 전체 밉
	std::vector<FD3D12DescriptorHandle> HzbMipUavs;       // 밉마다
	FMatrix4x4                          HzbViewProjection; // HZB를 만든 프레임의 뷰-투영
	bool                                bHzbValid = false;

	// 이번 프레임
	D3D12_GPU_VIRTUAL_ADDRESS ItemsAddress = 0;
	uint32                    ItemCount    = 0;
	uint32                    BatchCount   = 0;
	std::vector<FOcclusionItem> Items;
	std::vector<uint32>         TrianglesPerInstance; // 묶음별 (통계)

	// 통계 리드백 (프레임 슬롯별)
	ComPtr<ID3D12Resource> Readback[FD3D12RHI::FrameCount];
	uint64                 ReadbackCapacity[FD3D12RHI::FrameCount] = {};
	std::vector<uint32>    ReadbackTriangles[FD3D12RHI::FrameCount]; // 그 슬롯을 기록할 때의 묶음별 삼각형 수
	uint32                 ReadbackTested[FD3D12RHI::FrameCount] = {};
	uint32 StatTested    = 0;
	uint32 StatPhase1    = 0;
	uint32 StatPhase2    = 0;
	uint64 StatTriangles = 0;
};

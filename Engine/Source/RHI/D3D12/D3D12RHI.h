#pragma once

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "RHI/D3D12/D3D12HdrOutput.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12SwapChain.h"
#include "RHI/D3D12/D3D12UploadQueue.h"

#include <filesystem>
#include <functional>
#include <vector>

struct FD3D12RHIDesc
{
	HWND   WindowHandle      = nullptr;
	uint32 Width             = 0;
	uint32 Height            = 0;
	bool   bEnableDebugLayer = false;
	bool   bVSync            = true;
	uint64 DynamicBufferSize = 4 * 1024 * 1024; // 프레임당 동적 업로드 버퍼 크기
	uint32 SrvDescriptorCount = 4096;           // 셰이더 가시 CBV/SRV/UAV 힙 크기
	uint64 UploadRingSize     = FD3D12UploadQueue::DefaultRingSize; // 비동기 업로드 링 크기
};

// D3D12 렌더링 파사드: 디바이스/큐/스왑체인/깊이 버퍼와 프레임 단위 커맨드 리스트를 묶는다.
// 프레임 흐름: BeginFrame(클리어) → (드로우) → EndFrame(실행 + Present)
class FD3D12RHI
{
public:
	static constexpr uint32      FrameCount         = FD3D12SwapChain::BackBufferCount;
	static constexpr DXGI_FORMAT RenderTargetFormat = FD3D12SwapChain::RenderTargetViewFormat; // PSO RTV 포맷 (sRGB)
	static constexpr DXGI_FORMAT DepthBufferFormat  = FD3D12DepthBuffer::Format;

	~FD3D12RHI();

	bool Init(const FD3D12RHIDesc& Desc);
	void Shutdown();

	// 창 크기 변경 시 호출. GPU를 비운 뒤 백버퍼/깊이 버퍼를 재생성한다.
	void Resize(uint32 Width, uint32 Height);

	// 이번 프레임의 백버퍼를 렌더 타깃으로 전이하고 색/깊이를 클리어. 동적 업로드 버퍼도 초기화된다.
	void BeginFrame(const float ClearColor[4]);

	// 커맨드 리스트를 닫아 실행하고 Present
	void EndFrame();

	// 백버퍼(+깊이)를 렌더 타깃으로 다시 바인딩하고 전체 뷰포트/시저 설정.
	// 오프스크린 렌더 타깃을 쓴 뒤 복귀하거나, UI를 UNORM 뷰(bLinearView)로 그릴 때 사용.
	void SetRenderTargetToBackBuffer(bool bLinearView = false);

	// 이번 프레임의 최종 백버퍼를 PNG로 저장 (EndFrame에서 복사 후 GPU 완료를 기다려 기록 — 검증용)
	void RequestScreenshot(const std::filesystem::path& Path) { PendingScreenshot = Path; }

	// 현재 백버퍼의 sRGB RTV를 출력 대상으로 (BeginFrame 이후 유효)
	FRenderOutput GetBackBufferOutput() const;

	// 이번 프레임의 슬롯(백버퍼 칸, 프레임 리소스 인덱스)과 BeginFrame마다 1씩 느는 프레임 번호 (GPU 타이머 등)
	uint32 GetFrameSlot() const { return CurrentBackBufferIndex; }
	uint64 GetFrameNumber() const { return FrameNumber; }

	// ---- HDR 디스플레이 출력 (Phase 49, D3D12HdrOutput.h): 켜면 GetBackBufferOutput/SetRenderTargetToBackBuffer는 겹침 층(SDR UI·ImGui용),
	// 씬은 GetSceneOutput(선형 FP16)에 HDR 톤매핑으로 그린다. EndFrame이 합성해 HDR10/scRGB 스왑체인에 쓴다. 끄면 둘 다 기존 백버퍼
	// BeginFrame 전에만 부른다 (GPU를 비우고 스왑체인 포맷을 바꾼다). 디스플레이/스왑체인이 지원하지 않으면 false (SDR 유지)
	// MaxNits 0 = 디스플레이가 알려 주는 최대 밝기 (모르면 1000)
	bool                SetHdrOutput(EHdrSwapChainMode Mode, float PaperWhiteNits, float MaxNits = 0.0f);
	bool                IsHdrOutputActive() const { return HdrOutput.IsActive(); }
	EHdrSwapChainMode   GetHdrOutputMode() const { return HdrOutput.GetMode(); }
	float               GetHdrPaperWhiteNits() const { return HdrOutput.GetPaperWhiteNits(); }
	float               GetHdrMaxNits() const { return HdrOutput.GetMaxNits(); }
	FHdrDisplayInfo     QueryHdrDisplay() const { return SwapChain.QueryHdrDisplay(); }
	// 씬 렌더러 출력 (HDR이면 선형 FP16 씬 타깃, 아니면 GetBackBufferOutput과 같음)
	FRenderOutput       GetSceneOutput() const;
	ID3D12Resource*     GetHdrSceneResource() const { return HdrOutput.IsActive() && HdrOutput.GetSceneTarget() != nullptr ? HdrOutput.GetSceneTarget()->GetColorResource() : nullptr; }

	void SetVSync(bool bEnabled) { bVSync = bEnabled; }
	bool IsVSync() const { return bVSync; }

	FD3D12Device&              GetDevice() { return Device; }
	FD3D12CommandQueue&        GetGraphicsQueue() { return GraphicsQueue; }
	FD3D12CommandQueue&        GetComputeQueue() { return ComputeQueue; }
	FD3D12SwapChain&           GetSwapChain() { return SwapChain; }
	// 프레임 그래픽스 명령 목록. SubmitGraphicsCommands로 중간 제출해도 같은 객체(새 할당자로 Reset)이므로 포인터를 들고 있어도 된다
	ID3D12GraphicsCommandList* GetCommandList() const { return CommandList.Get(); }

	// ---- 프레임 중간 제출 / 비동기 계산 (렌더 그래프 — Renderer/RenderGraph/RenderGraph.h)
	// 지금까지 기록한 그래픽스 명령을 닫아 실행하고 그래픽스 큐 펜스 값을 돌려준다. 명령 목록은 같은 프레임 슬롯의 새 할당자로
	// 다시 열리고 셰이더 가시 힙이 다시 바인딩된다 (그 밖의 파이프라인 상태·렌더 타깃 바인딩은 사라진다)
	uint64 SubmitGraphicsCommands();
	// 계산 큐 명령 목록 기록 시작: 계산 큐가 그래픽스 펜스 WaitGraphicsFence(0이면 대기 없음)를 기다린 뒤 실행된다
	ID3D12GraphicsCommandList* BeginComputeCommands(uint64 WaitGraphicsFence);
	// 계산 명령을 제출하고 계산 큐 펜스 값을 돌려준다 (이후 그래픽스 큐가 WaitForCompute로 기다린다)
	uint64 SubmitComputeCommands();
	// 이후 그래픽스 큐 제출이 계산 큐 펜스 값까지 기다리게 한다 (보통 SubmitGraphicsCommands 직후)
	void WaitForCompute(uint64 ComputeFence);
	// 이번 프레임 그래픽스 명령 목록 제출 횟수 (EndFrame 포함, 통계용)
	uint32 GetGraphicsSubmitCount() const { return GraphicsSubmitsThisFrame; }

	// 현재 프레임의 동적 업로드 버퍼 (BeginFrame 이후 유효)
	FD3D12DynamicUploadBuffer& GetDynamicBuffer() { return DynamicBuffers[CurrentBackBufferIndex]; }

	// 셰이더 가시 CBV/SRV/UAV 디스크립터 할당자. BeginFrame에서 힙이 바인딩된다.
	FD3D12DescriptorAllocator& GetSrvAllocator() { return SrvAllocator; }

	// 비동기 업로드 (복사 큐 + 업로드 링). 제출/완료 확인/텍스처 전이는 BeginFrame이 한다 — D3D12UploadQueue.h 머리 주석
	FD3D12UploadQueue& GetUploadQueue() { return UploadQueue; }
	// 업로드를 지금 끝낸다 (CPU 대기): 제출 → 복사 완료 대기 → 텍스처 전이를 즉시 실행 명령 리스트로 → 그 완료까지 대기.
	// 동기 로딩/자동 검증 비우기용. 프레임 기록 중에 불러도 된다 (프레임 리스트보다 먼저 실행됨)
	void FlushUploads();
	// BeginFrame 안(명령 리스트 기록 시작 직후, 업로드 전이 기록 뒤)에 불리는 콜백. 반환 = 제거용 ID
	uint32 AddBeginFrameCallback(std::function<void()> Callback);
	void   RemoveBeginFrameCallback(uint32 Id);

	// 지연 해제: 요청한 뒤 처음 제출되는 프레임(EndFrame)을 GPU가 끝낸 뒤 실제로 해제한다.
	// BeginFrame 전(UI 단계)에 요청해도 그 프레임이 아직 쓰는 리소스를 먼저 지우지 않는다
	void DeferRelease(ComPtr<ID3D12Object> Object);
	void DeferFreeDescriptor(const FD3D12DescriptorHandle& Handle);

private:
	struct FPendingReleases
	{
		std::vector<ComPtr<ID3D12Object>>   Objects;
		std::vector<FD3D12DescriptorHandle> SrvDescriptors;
	};

	void ProcessPendingReleases(FPendingReleases& Pending);
	// 프레임 슬롯의 할당자 풀에서 다음 할당자 (없으면 만든다)
	ID3D12CommandAllocator* AcquireAllocator(std::vector<ComPtr<ID3D12CommandAllocator>>& Pool, uint32& Used, D3D12_COMMAND_LIST_TYPE Type,
	                                         const wchar_t* Prefix);
	// 업로드 큐 제출 + 그래픽스 큐가 업로드 완료를 기다리게 (프레임 명령 제출 직전마다)
	void WaitForUploadsBeforeSubmit();

	FD3D12Device              Device;
	FD3D12CommandQueue        GraphicsQueue;
	FD3D12CommandQueue        ComputeQueue; // 비동기 계산 (렌더 그래프)
	FD3D12SwapChain           SwapChain;
	FD3D12DepthBuffer         DepthBuffer;
	FD3D12DescriptorAllocator SrvAllocator;
	FD3D12UploadQueue         UploadQueue;
	FD3D12HdrOutput           HdrOutput; // HDR 출력 (켜졌을 때만 리소스)
	std::vector<std::pair<uint32, std::function<void()>>> BeginFrameCallbacks;
	uint32                                                NextBeginFrameCallbackId = 1;

	// 백버퍼마다 별도 할당자/동적 버퍼: GPU가 사용 중인 프레임의 메모리를 덮어쓰지 않기 위함.
	// 할당자는 슬롯마다 풀 (프레임 중간 제출마다 하나씩 더 쓴다 — BeginFrame에서 그 슬롯의 쓴 것만 Reset)
	std::vector<ComPtr<ID3D12CommandAllocator>> CommandAllocators[FrameCount];
	std::vector<ComPtr<ID3D12CommandAllocator>> ComputeAllocators[FrameCount];
	uint32                            UsedCommandAllocators[FrameCount] = {};
	uint32                            UsedComputeAllocators[FrameCount] = {};
	FD3D12DynamicUploadBuffer         DynamicBuffers[FrameCount];
	FPendingReleases                  PendingReleases[FrameCount];
	FPendingReleases                  RecordingReleases; // 다음 EndFrame에 제출 프레임 칸으로 옮겨짐
	ComPtr<ID3D12GraphicsCommandList> CommandList;
	ComPtr<ID3D12GraphicsCommandList> ComputeCommandList;
	bool                              bComputeRecording = false;
	uint64                            FrameFenceValues[FrameCount]   = {};
	uint64                            ComputeFenceValues[FrameCount] = {}; // 슬롯이 마지막으로 제출한 계산 펜스 값
	uint32                            GraphicsSubmitsThisFrame       = 0;

	std::filesystem::path PendingScreenshot;
	bool                  WriteScreenshot(ID3D12Resource* Readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint);

	uint32 CurrentBackBufferIndex = 0;
	uint64 FrameNumber            = 0;
	bool   bVSync                 = true;
	bool   bInitialized           = false;
};

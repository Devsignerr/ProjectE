#pragma once

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12SwapChain.h"

#include <filesystem>
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

	void SetVSync(bool bEnabled) { bVSync = bEnabled; }
	bool IsVSync() const { return bVSync; }

	FD3D12Device&              GetDevice() { return Device; }
	FD3D12CommandQueue&        GetGraphicsQueue() { return GraphicsQueue; }
	FD3D12SwapChain&           GetSwapChain() { return SwapChain; }
	ID3D12GraphicsCommandList* GetCommandList() const { return CommandList.Get(); }

	// 현재 프레임의 동적 업로드 버퍼 (BeginFrame 이후 유효)
	FD3D12DynamicUploadBuffer& GetDynamicBuffer() { return DynamicBuffers[CurrentBackBufferIndex]; }

	// 셰이더 가시 CBV/SRV/UAV 디스크립터 할당자. BeginFrame에서 힙이 바인딩된다.
	FD3D12DescriptorAllocator& GetSrvAllocator() { return SrvAllocator; }

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

	FD3D12Device              Device;
	FD3D12CommandQueue        GraphicsQueue;
	FD3D12SwapChain           SwapChain;
	FD3D12DepthBuffer         DepthBuffer;
	FD3D12DescriptorAllocator SrvAllocator;

	// 백버퍼마다 별도 할당자/동적 버퍼: GPU가 사용 중인 프레임의 메모리를 덮어쓰지 않기 위함
	ComPtr<ID3D12CommandAllocator>    CommandAllocators[FrameCount];
	FD3D12DynamicUploadBuffer         DynamicBuffers[FrameCount];
	FPendingReleases                  PendingReleases[FrameCount];
	FPendingReleases                  RecordingReleases; // 다음 EndFrame에 제출 프레임 칸으로 옮겨짐
	ComPtr<ID3D12GraphicsCommandList> CommandList;
	uint64                            FrameFenceValues[FrameCount] = {};

	std::filesystem::path PendingScreenshot;
	bool                  WriteScreenshot(ID3D12Resource* Readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint);

	uint32 CurrentBackBufferIndex = 0;
	uint64 FrameNumber            = 0;
	bool   bVSync                 = true;
	bool   bInitialized           = false;
};

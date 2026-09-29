#include "RHI/D3D12/D3D12RHI.h"

FD3D12RHI::~FD3D12RHI()
{
	Shutdown();
}

bool FD3D12RHI::Init(const FD3D12RHIDesc& Desc)
{
	E_CHECKF(!bInitialized, "FD3D12RHI가 이미 초기화되어 있습니다");
	E_CHECKF(Desc.WindowHandle != nullptr, "유효한 창 핸들이 필요합니다");

	bVSync = Desc.bVSync;

	if (!Device.Init(Desc.bEnableDebugLayer))
	{
		return false;
	}

	ID3D12Device* D3DDevice = Device.GetDevice();

	if (!GraphicsQueue.Init(D3DDevice, D3D12_COMMAND_LIST_TYPE_DIRECT))
	{
		return false;
	}

	if (!SwapChain.Init(Device, GraphicsQueue, Desc.WindowHandle, Desc.Width, Desc.Height))
	{
		return false;
	}

	if (!DepthBuffer.Init(D3DDevice, Desc.Width, Desc.Height))
	{
		return false;
	}

	if (!SrvAllocator.Init(D3DDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Desc.SrvDescriptorCount, true, L"SrvDescriptorHeap"))
	{
		return false;
	}

	for (uint32 Index = 0; Index < FrameCount; ++Index)
	{
		E_D3D_VERIFY(D3DDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CommandAllocators[Index])));
		CommandAllocators[Index]->SetName(std::format(L"CommandAllocator_{}", Index).c_str());

		if (!DynamicBuffers[Index].Init(D3DDevice, Desc.DynamicBufferSize, std::format(L"DynamicUploadBuffer_{}", Index).c_str()))
		{
			return false;
		}
	}

	E_D3D_VERIFY(D3DDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CommandAllocators[0].Get(), nullptr,
	                                          IID_PPV_ARGS(&CommandList)));
	CommandList->SetName(L"GraphicsCommandList");

	// 생성 직후는 기록 상태이므로 닫아 두어야 BeginFrame에서 Reset할 수 있다
	E_D3D_VERIFY(CommandList->Close());

	bInitialized = true;
	E_LOG(LogD3D12, Display, "D3D12 RHI 초기화 완료");
	return true;
}

void FD3D12RHI::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	// GPU가 모든 리소스 사용을 끝낸 뒤 해제
	GraphicsQueue.Flush();

	CommandList.Reset();
	for (uint32 Index = 0; Index < FrameCount; ++Index)
	{
		ProcessPendingReleases(PendingReleases[Index]);
		CommandAllocators[Index].Reset();
		DynamicBuffers[Index].Shutdown();
	}

	SrvAllocator.Shutdown();
	DepthBuffer.Shutdown();
	SwapChain.Shutdown();
	GraphicsQueue.Shutdown();
	Device.Shutdown();

	bInitialized = false;
	E_LOG(LogD3D12, Display, "D3D12 RHI 종료 완료");
}

void FD3D12RHI::Resize(uint32 Width, uint32 Height)
{
	if (!bInitialized || Width == 0 || Height == 0)
	{
		return;
	}
	if (Width == SwapChain.GetWidth() && Height == SwapChain.GetHeight())
	{
		return;
	}

	// 백버퍼/깊이 버퍼를 참조 중인 GPU 작업이 모두 끝나야 재생성할 수 있다
	GraphicsQueue.Flush();

	if (!SwapChain.Resize(Width, Height))
	{
		E_LOG(LogD3D12, Fatal, "스왑체인 리사이즈 실패");
	}
	if (!DepthBuffer.Resize(Device.GetDevice(), Width, Height))
	{
		E_LOG(LogD3D12, Fatal, "깊이 버퍼 리사이즈 실패");
	}
}

void FD3D12RHI::BeginFrame(const float ClearColor[4])
{
	CurrentBackBufferIndex = SwapChain.GetCurrentBackBufferIndex();

	// 이 백버퍼를 마지막으로 사용한 프레임이 GPU에서 끝날 때까지 대기
	GraphicsQueue.WaitForFenceValue(FrameFenceValues[CurrentBackBufferIndex]);

	// 이 인덱스의 이전 프레임에서 예약된 해제를 실행 (GPU 사용 완료 보장됨)
	ProcessPendingReleases(PendingReleases[CurrentBackBufferIndex]);
	DynamicBuffers[CurrentBackBufferIndex].Reset();

	ID3D12CommandAllocator* Allocator = CommandAllocators[CurrentBackBufferIndex].Get();
	E_D3D_CHECK(Allocator->Reset());
	E_D3D_CHECK(CommandList->Reset(Allocator, nullptr));

	// 셰이더 가시 힙 바인딩 (프레임 내내 동일)
	ID3D12DescriptorHeap* DescriptorHeaps[] = { SrvAllocator.GetHeap() };
	CommandList->SetDescriptorHeaps(1, DescriptorHeaps);

	ID3D12Resource* BackBuffer = SwapChain.GetCurrentBackBuffer();

	const D3D12_RESOURCE_BARRIER ToRenderTarget =
		MakeTransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	CommandList->ResourceBarrier(1, &ToRenderTarget);

	const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = SwapChain.GetCurrentRenderTargetView();
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DepthBuffer.GetDepthStencilView();
	CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);
	CommandList->ClearRenderTargetView(Rtv, ClearColor, 0, nullptr);
	CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, FD3D12DepthBuffer::ClearDepth, 0, 0, nullptr);

	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f,
	                               static_cast<float>(SwapChain.GetWidth()), static_cast<float>(SwapChain.GetHeight()),
	                               D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	const D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(SwapChain.GetWidth()), static_cast<LONG>(SwapChain.GetHeight()) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
}

void FD3D12RHI::DeferRelease(ComPtr<ID3D12Object> Object)
{
	if (Object)
	{
		PendingReleases[CurrentBackBufferIndex].Objects.push_back(std::move(Object));
	}
}

void FD3D12RHI::DeferFreeDescriptor(const FD3D12DescriptorHandle& Handle)
{
	if (Handle.IsValid())
	{
		PendingReleases[CurrentBackBufferIndex].SrvDescriptors.push_back(Handle);
	}
}

void FD3D12RHI::ProcessPendingReleases(FPendingReleases& Pending)
{
	Pending.Objects.clear();
	for (FD3D12DescriptorHandle& Handle : Pending.SrvDescriptors)
	{
		SrvAllocator.Free(Handle);
	}
	Pending.SrvDescriptors.clear();
}

void FD3D12RHI::EndFrame()
{
	ID3D12Resource* BackBuffer = SwapChain.GetCurrentBackBuffer();

	const D3D12_RESOURCE_BARRIER ToPresent =
		MakeTransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	CommandList->ResourceBarrier(1, &ToPresent);

	E_D3D_CHECK(CommandList->Close());

	FrameFenceValues[CurrentBackBufferIndex] = GraphicsQueue.ExecuteCommandList(CommandList.Get());

	SwapChain.Present(bVSync);
}

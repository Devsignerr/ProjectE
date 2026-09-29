#include "RHI/D3D12/D3D12SwapChain.h"

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Device.h"

bool FD3D12SwapChain::Init(FD3D12Device& InDevice, FD3D12CommandQueue& PresentQueue, HWND WindowHandle,
                           uint32 InWidth, uint32 InHeight)
{
	Device = &InDevice;
	Width  = InWidth;
	Height = InHeight;

	// 테어링(VSync 끔) 지원 시 플래그 필요. Present 시에도 동일 플래그 사용.
	SwapChainFlags = Device->IsTearingSupported() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	DXGI_SWAP_CHAIN_DESC1 SwapChainDesc{};
	SwapChainDesc.Width       = Width;
	SwapChainDesc.Height      = Height;
	SwapChainDesc.Format      = BackBufferFormat;
	SwapChainDesc.Stereo      = FALSE;
	SwapChainDesc.SampleDesc  = { 1, 0 };
	SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	SwapChainDesc.BufferCount = BackBufferCount;
	SwapChainDesc.Scaling     = DXGI_SCALING_STRETCH;
	SwapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	SwapChainDesc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
	SwapChainDesc.Flags       = SwapChainFlags;

	IDXGIFactory6* Factory = Device->GetFactory();

	ComPtr<IDXGISwapChain1> SwapChain1;
	E_D3D_VERIFY(Factory->CreateSwapChainForHwnd(PresentQueue.GetQueue(), WindowHandle, &SwapChainDesc,
	                                             nullptr, nullptr, &SwapChain1));

	// Alt+Enter 전체화면 전환은 엔진이 직접 처리 (DXGI 기본 동작 비활성화)
	E_D3D_VERIFY(Factory->MakeWindowAssociation(WindowHandle, DXGI_MWA_NO_ALT_ENTER));
	E_D3D_VERIFY(SwapChain1.As(&SwapChain));

	// 백버퍼마다 sRGB 뷰 + UNORM 뷰
	if (!RtvHeap.Init(Device->GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BackBufferCount * 2, false, L"SwapChainRtvHeap"))
	{
		return false;
	}

	if (!CreateBackBufferViews())
	{
		return false;
	}

	E_LOG(LogD3D12, Display, "스왑체인 생성 완료: {}x{}, 백버퍼 {}개", Width, Height, BackBufferCount);
	return true;
}

void FD3D12SwapChain::Shutdown()
{
	ReleaseBackBuffers();
	RtvHeap.Shutdown();
	SwapChain.Reset();
	Device = nullptr;
}

bool FD3D12SwapChain::Resize(uint32 InWidth, uint32 InHeight)
{
	E_CHECKF(InWidth > 0 && InHeight > 0, "스왑체인 크기는 0일 수 없습니다");

	ReleaseBackBuffers();

	E_D3D_VERIFY(SwapChain->ResizeBuffers(BackBufferCount, InWidth, InHeight, BackBufferFormat, SwapChainFlags));

	Width  = InWidth;
	Height = InHeight;

	if (!CreateBackBufferViews())
	{
		return false;
	}

	E_LOG(LogD3D12, Log, "스왑체인 리사이즈: {}x{}", Width, Height);
	return true;
}

void FD3D12SwapChain::Present(bool bVSync)
{
	const UINT SyncInterval = bVSync ? 1 : 0;
	const UINT PresentFlags = (!bVSync && (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) ? DXGI_PRESENT_ALLOW_TEARING : 0;

	const HRESULT Result = SwapChain->Present(SyncInterval, PresentFlags);
	if (Result == DXGI_ERROR_DEVICE_REMOVED || Result == DXGI_ERROR_DEVICE_RESET)
	{
		const HRESULT Reason = Device->GetDevice()->GetDeviceRemovedReason();
		E_LOG(LogD3D12, Fatal, "GPU 디바이스가 제거되었습니다 (사유: {})", HResultToString(Reason));
	}
	else if (FAILED(Result))
	{
		E_LOG(LogD3D12, Fatal, "Present 실패: {}", HResultToString(Result));
	}
}

bool FD3D12SwapChain::CreateBackBufferViews()
{
	ID3D12Device* D3DDevice = Device->GetDevice();

	for (uint32 Index = 0; Index < BackBufferCount; ++Index)
	{
		E_D3D_VERIFY(SwapChain->GetBuffer(Index, IID_PPV_ARGS(&BackBuffers[Index])));
		BackBuffers[Index]->SetName(std::format(L"BackBuffer_{}", Index).c_str());

		D3D12_RENDER_TARGET_VIEW_DESC RtvDesc{};
		RtvDesc.Format             = RenderTargetViewFormat;
		RtvDesc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
		RtvDesc.Texture2D.MipSlice = 0;
		D3DDevice->CreateRenderTargetView(BackBuffers[Index].Get(), &RtvDesc, RtvHeap.GetCpuHandle(Index));

		RtvDesc.Format = BackBufferFormat;
		D3DDevice->CreateRenderTargetView(BackBuffers[Index].Get(), &RtvDesc, RtvHeap.GetCpuHandle(BackBufferCount + Index));
	}

	return true;
}

void FD3D12SwapChain::ReleaseBackBuffers()
{
	for (ComPtr<ID3D12Resource>& BackBuffer : BackBuffers)
	{
		BackBuffer.Reset();
	}
}

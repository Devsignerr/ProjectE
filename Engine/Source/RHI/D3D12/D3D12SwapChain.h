#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

class FD3D12Device;
class FD3D12CommandQueue;

// 창에 연결된 스왑체인과 백버퍼 RTV
class FD3D12SwapChain
{
public:
	static constexpr uint32      BackBufferCount  = 3;
	static constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // 플립 모델은 비-sRGB 리소스 포맷 필요
	// RTV는 sRGB 뷰로 만들어 셰이더의 선형 출력이 자동 감마 인코딩되게 한다. PSO의 RTV 포맷도 이 값을 쓴다.
	static constexpr DXGI_FORMAT RenderTargetViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

	bool Init(FD3D12Device& InDevice, FD3D12CommandQueue& PresentQueue, HWND WindowHandle, uint32 InWidth, uint32 InHeight);
	void Shutdown();

	// 백버퍼 재생성. 호출 전에 GPU가 백버퍼 사용을 끝냈음을 보장(Flush)해야 한다.
	bool Resize(uint32 InWidth, uint32 InHeight);

	void Present(bool bVSync);

	uint32                      GetCurrentBackBufferIndex() const { return SwapChain->GetCurrentBackBufferIndex(); }
	ID3D12Resource*             GetCurrentBackBuffer() const { return BackBuffers[GetCurrentBackBufferIndex()].Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView() const { return RtvHeap.GetCpuHandle(GetCurrentBackBufferIndex()); }

	uint32 GetWidth() const { return Width; }
	uint32 GetHeight() const { return Height; }

private:
	bool CreateBackBufferViews();
	void ReleaseBackBuffers();

	FD3D12Device*           Device = nullptr; // 소유하지 않음 (FD3D12RHI가 수명 관리)
	ComPtr<IDXGISwapChain4> SwapChain;
	ComPtr<ID3D12Resource>  BackBuffers[BackBufferCount];
	FD3D12DescriptorHeap    RtvHeap;

	uint32 Width          = 0;
	uint32 Height         = 0;
	UINT   SwapChainFlags = 0;
};

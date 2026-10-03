#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

#include <string>

class FD3D12Device;
class FD3D12CommandQueue;

// 창이 있는 디스플레이의 HDR 정보 (IDXGIOutput6::GetDesc1, Phase 49)
struct FHdrDisplayInfo
{
	bool        bValid           = false; // 출력 정보를 얻었다
	bool        bHdrEnabled      = false; // OS에서 HDR이 켜져 있다 (색공간 ST.2084/BT.2020)
	bool        bHdr10Supported  = false; // 스왑체인이 HDR10 색공간으로 Present할 수 있다
	bool        bScRgbSupported  = false; // 스왑체인이 scRGB(선형 FP16)로 Present할 수 있다
	float       MaxNits          = 0.0f;
	float       MinNits          = 0.0f;
	float       MaxFullFrameNits = 0.0f;
	uint32      BitsPerColor     = 0;
	std::string DeviceName;
};

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

	uint32          GetCurrentBackBufferIndex() const { return SwapChain->GetCurrentBackBufferIndex(); }
	ID3D12Resource* GetCurrentBackBuffer() const { return BackBuffers[GetCurrentBackBufferIndex()].Get(); }

	// bLinearView = false: sRGB 뷰(씬 렌더링용, 자동 감마 인코딩)
	// bLinearView = true : UNORM 뷰(UI 등 이미 sRGB로 인코딩된 색을 그대로 쓸 때)
	D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView(bool bLinearView = false) const
	{
		return RtvHeap.GetCpuHandle(GetCurrentBackBufferIndex() + (bLinearView ? BackBufferCount : 0));
	}

	uint32 GetWidth() const { return Width; }
	uint32 GetHeight() const { return Height; }

	// HDR 출력 (Phase 49): 백버퍼 포맷·색공간 바꾸기 (R10G10B10A2 + ST.2084/BT.2020, R16G16B16A16_FLOAT + 선형 BT.709, 또는 기본 SDR).
	// 호출 전 GPU가 백버퍼를 다 썼어야 한다 (Flush). 색공간을 지원하지 않으면 false (바꾸지 않음). 두 RTV 칸 모두 그 포맷
	bool                  SetFormat(DXGI_FORMAT Format, DXGI_COLOR_SPACE_TYPE ColorSpace);
	DXGI_FORMAT           GetFormat() const { return CurrentFormat; }
	DXGI_COLOR_SPACE_TYPE GetColorSpace() const { return CurrentColorSpace; }
	bool                  SupportsColorSpace(DXGI_COLOR_SPACE_TYPE ColorSpace) const;
	// 창이 들어 있는 디스플레이의 HDR 정보
	FHdrDisplayInfo QueryHdrDisplay() const;

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
	DXGI_FORMAT           CurrentFormat     = BackBufferFormat;
	DXGI_COLOR_SPACE_TYPE CurrentColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
};

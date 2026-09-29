#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

class FD3D12Device;
class FD3D12RHI;

// 오프스크린 색상+깊이 렌더 타깃 (에디터 뷰포트 등).
// 색상 리소스는 TYPELESS: RTV는 sRGB 뷰(선형 → 감마 인코딩), SRV는 UNORM 뷰(UI가 인코딩된 값을 그대로 표시).
class FD3D12RenderTarget
{
public:
	static constexpr DXGI_FORMAT ResourceFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	static constexpr DXGI_FORMAT RtvFormat      = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	static constexpr DXGI_FORMAT SrvFormat      = DXGI_FORMAT_R8G8B8A8_UNORM;

	~FD3D12RenderTarget();

	bool Init(FD3D12Device& Device, FD3D12DescriptorAllocator& InSrvAllocator, uint32 InWidth, uint32 InHeight,
	          const wchar_t* DebugName);
	void Shutdown();
	void ShutdownDeferred(FD3D12RHI& Rhi);

	// 렌더 타깃 상태로 전이 + 바인딩 + 클리어 + 뷰포트/시저
	void Begin(ID3D12GraphicsCommandList* CommandList, const float ClearColor[4]);
	// 픽셀 셰이더 리소스 상태로 전이 (이후 SRV로 샘플링 가능)
	void End(ID3D12GraphicsCommandList* CommandList);

	const FD3D12DescriptorHandle& GetSrv() const { return Srv; }
	ID3D12Resource*               GetColorResource() const { return ColorResource.Get(); }
	uint32                        GetWidth() const { return Width; }
	uint32                        GetHeight() const { return Height; }

private:
	ComPtr<ID3D12Resource>     ColorResource;
	FD3D12DescriptorHeap       RtvHeap;
	FD3D12DescriptorHandle     Srv;
	FD3D12DescriptorAllocator* SrvAllocator = nullptr;
	FD3D12DepthBuffer          DepthBuffer;
	uint32                     Width  = 0;
	uint32                     Height = 0;
	bool                       bInRenderState = false;
};

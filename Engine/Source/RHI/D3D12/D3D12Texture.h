#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"

class FD3D12CommandQueue;
class FD3D12Device;
class FD3D12RHI;

// GPU 2D 텍스처 + SRV. 초기 데이터를 업로드 힙을 거쳐 동기 복사한다 (로딩 시점용).
class FD3D12Texture
{
public:
	~FD3D12Texture();

	// Pixels: 행 단위로 빈틈없이 채워진 (Width * BytesPerPixel) 데이터. 현재 밉 1개만 지원.
	bool Init2D(FD3D12Device& Device, FD3D12CommandQueue& Queue, FD3D12DescriptorAllocator& InSrvAllocator,
	            uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const void* Pixels, uint32 BytesPerPixel,
	            const wchar_t* DebugName);
	// 즉시 해제 (GPU가 더 이상 사용하지 않음이 보장될 때)
	void Shutdown();
	// 지연 해제 (렌더링 중 교체/삭제 시)
	void ShutdownDeferred(FD3D12RHI& Rhi);

	const FD3D12DescriptorHandle& GetSrv() const { return Srv; }
	ID3D12Resource*               GetResource() const { return Resource.Get(); }
	uint32                        GetWidth() const { return Width; }
	uint32                        GetHeight() const { return Height; }
	DXGI_FORMAT                   GetFormat() const { return Format; }

private:
	ComPtr<ID3D12Resource>     Resource;
	FD3D12DescriptorHandle     Srv;
	FD3D12DescriptorAllocator* SrvAllocator = nullptr; // 소유하지 않음
	uint32                     Width        = 0;
	uint32                     Height       = 0;
	DXGI_FORMAT                Format       = DXGI_FORMAT_UNKNOWN;
};

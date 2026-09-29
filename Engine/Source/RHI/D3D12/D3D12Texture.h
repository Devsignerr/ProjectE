#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"

class FD3D12CommandQueue;
class FD3D12Device;
class FD3D12RHI;

// GPU 2D 텍스처 + SRV. 초기 데이터를 업로드 힙을 거쳐 동기 복사하고, 필요 시 컴퓨트로 밉 체인을 생성한다 (로딩 시점용).
class FD3D12Texture
{
public:
	~FD3D12Texture();

	// Pixels: 행 단위로 빈틈없이 채워진 (Width * BytesPerPixel) 밉 0 데이터.
	// bGenerateMips: 지원 포맷(R8G8B8A8/B8G8R8A8 UNORM·sRGB)이면 전체 밉 체인을 GPU에서 생성. 그 외 포맷은 밉 1개.
	bool Init2D(FD3D12Device& Device, FD3D12CommandQueue& Queue, FD3D12DescriptorAllocator& InSrvAllocator,
	            uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const void* Pixels, uint32 BytesPerPixel,
	            const wchar_t* DebugName, bool bGenerateMips = true);
	// 즉시 해제 (GPU가 더 이상 사용하지 않음이 보장될 때)
	void Shutdown();
	// 지연 해제 (렌더링 중 교체/삭제 시)
	void ShutdownDeferred(FD3D12RHI& Rhi);

	const FD3D12DescriptorHandle& GetSrv() const { return Srv; }
	ID3D12Resource*               GetResource() const { return Resource.Get(); }
	uint32                        GetWidth() const { return Width; }
	uint32                        GetHeight() const { return Height; }
	uint32                        GetMipCount() const { return MipCount; }
	DXGI_FORMAT                   GetFormat() const { return Format; } // 뷰 포맷 (리소스는 TYPELESS일 수 있음)

private:
	ComPtr<ID3D12Resource>     Resource;
	FD3D12DescriptorHandle     Srv;
	FD3D12DescriptorAllocator* SrvAllocator = nullptr; // 소유하지 않음
	uint32                     Width        = 0;
	uint32                     Height       = 0;
	uint32                     MipCount     = 1;
	DXGI_FORMAT                Format       = DXGI_FORMAT_UNKNOWN;
};

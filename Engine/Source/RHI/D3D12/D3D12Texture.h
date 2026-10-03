#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"

class FD3D12CommandQueue;
class FD3D12Device;
class FD3D12RHI;
class FD3D12UploadQueue;

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
	// 미리 만든 밉 체인 업로드 (BC 압축 포맷 포함). Mips[i]: 밉 i 데이터, 행(블록 포맷이면 블록 행) 단위로 빈틈없이 채움
	struct FMipData
	{
		const void* Data = nullptr;
		size_t      Size = 0;
	};
	bool Init2DFromMips(FD3D12Device& Device, FD3D12CommandQueue& Queue, FD3D12DescriptorAllocator& InSrvAllocator,
	                    uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const FMipData* Mips, uint32 InMipCount,
	                    const wchar_t* DebugName);
	// 비동기 업로드 (복사 큐): 리소스/SRV는 바로 만들고 데이터는 Uploader의 열린 묶음에 기록한다.
	// 그 묶음이 끝나고 RHI가 전이를 기록하면(펜스 <= GetFinalizedFence) 소유자가 MarkUploadComplete를 부른다 — 그 전에는 IsReady가 false
	bool Init2DFromMipsAsync(FD3D12Device& Device, FD3D12UploadQueue& Uploader, FD3D12DescriptorAllocator& InSrvAllocator, uint32 InWidth,
	                         uint32 InHeight, DXGI_FORMAT InFormat, const FMipData* Mips, uint32 InMipCount, const wchar_t* DebugName);
	// 셰이더가 읽어도 되는지 (리소스가 있고 비동기 업로드가 끝남). 기본 생성(로딩 중 자리표시)이면 false
	bool   IsReady() const { return Resource != nullptr && !bUploadPending; }
	bool   IsUploadPending() const { return bUploadPending; }
	uint64 GetUploadFence() const { return UploadFence; }
	void   MarkUploadComplete() { bUploadPending = false; }

	// 내용(리소스·SRV·크기·상태)을 맞바꾼다 — 텍스처 밉 스트리밍이 같은 핸들 뒤의 리소스를 새 밉 범위로 교체할 때.
	// 교체 뒤 Other(이전 내용)는 ShutdownDeferred로 지연 해제한다
	void SwapContents(FD3D12Texture& Other) noexcept;

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
	uint64                     UploadFence  = 0; // 비동기 업로드 묶음 펜스 (0 = 동기 업로드)
	bool                       bUploadPending = false;
};

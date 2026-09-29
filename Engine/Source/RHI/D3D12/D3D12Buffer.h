#pragma once

#include "RHI/D3D12/D3D12Common.h"

class FD3D12CommandQueue;
class FD3D12Device;
class FD3D12RHI;

// GPU 전용(DEFAULT 힙) 버퍼. 정점/인덱스 등 정적 데이터용.
class FD3D12Buffer
{
public:
	~FD3D12Buffer();

	// 버퍼를 생성하고 Data를 업로드 힙을 거쳐 복사한다. 복사 완료까지 동기 대기하므로 로딩 시점에서만 사용.
	// (비동기 업로드 컨텍스트는 Phase 3 리소스 관리자에서 도입)
	bool InitStatic(FD3D12Device& Device, FD3D12CommandQueue& Queue, const void* Data, uint64 InSizeInBytes,
	                const wchar_t* DebugName);
	void Shutdown();
	// 지연 해제 (렌더링 중 교체/삭제 시)
	void ShutdownDeferred(FD3D12RHI& Rhi);

	ID3D12Resource*           GetResource() const { return Resource.Get(); }
	uint64                    GetSize() const { return SizeInBytes; }
	D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const { return Resource->GetGPUVirtualAddress(); }

	D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView(uint32 StrideInBytes) const;
	D3D12_INDEX_BUFFER_VIEW  GetIndexBufferView(DXGI_FORMAT Format = DXGI_FORMAT_R32_UINT) const;

private:
	ComPtr<ID3D12Resource> Resource;
	uint64                 SizeInBytes = 0;
};

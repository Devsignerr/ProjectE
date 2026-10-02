#pragma once

#include "RHI/D3D12/D3D12Common.h"

class FD3D12CommandQueue;
class FD3D12Device;
class FD3D12RHI;
class FD3D12UploadQueue;

// GPU 전용(DEFAULT 힙) 버퍼. 정점/인덱스 등 정적 데이터용.
class FD3D12Buffer
{
public:
	~FD3D12Buffer();

	// 버퍼를 생성하고 Data를 업로드 힙을 거쳐 복사한다. 복사 완료까지 동기 대기하므로 로딩 시점에서만 사용.
	// (비동기 업로드 컨텍스트는 Phase 3 리소스 관리자에서 도입)
	bool InitStatic(FD3D12Device& Device, FD3D12CommandQueue& Queue, const void* Data, uint64 InSizeInBytes,
	                const wchar_t* DebugName);
	// 비동기: 데이터를 Uploader의 열린 묶음(복사 큐)에 기록한다. 버퍼는 COMMON — 끝난 뒤 직접 큐에서 암시적 승격으로 쓴다.
	// GetUploadFence() <= Uploader.GetFinalizedFence()가 되기 전에는 그리지 않는다 (FStaticMesh::IsReady)
	bool   InitStaticAsync(FD3D12Device& Device, FD3D12UploadQueue& Uploader, const void* Data, uint64 InSizeInBytes, const wchar_t* DebugName);
	uint64 GetUploadFence() const { return UploadFence; }
	void   Shutdown();
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
	uint64                 UploadFence = 0; // 0 = 동기 업로드
};

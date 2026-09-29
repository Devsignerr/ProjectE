#pragma once

#include "RHI/D3D12/D3D12Common.h"

// 고정 크기 디스크립터 힙. 인덱스로 CPU/GPU 핸들을 계산한다.
// (동적 할당/해제는 이후 단계의 디스크립터 할당자에서 처리)
class FD3D12DescriptorHeap
{
public:
	bool Init(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 InCapacity, bool bInShaderVisible,
	          const wchar_t* DebugName);
	void Shutdown();

	D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(uint32 Index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(uint32 Index) const; // 셰이더 가시 힙에서만 유효

	ID3D12DescriptorHeap* GetHeap() const { return Heap.Get(); }
	uint32                GetCapacity() const { return Capacity; }

private:
	ComPtr<ID3D12DescriptorHeap> Heap;
	D3D12_CPU_DESCRIPTOR_HANDLE  CpuStart{};
	D3D12_GPU_DESCRIPTOR_HANDLE  GpuStart{};
	uint32                       IncrementSize  = 0;
	uint32                       Capacity       = 0;
	bool                         bShaderVisible = false;
};

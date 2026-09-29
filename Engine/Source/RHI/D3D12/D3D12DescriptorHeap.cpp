#include "RHI/D3D12/D3D12DescriptorHeap.h"

bool FD3D12DescriptorHeap::Init(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 InCapacity,
                                bool bInShaderVisible, const wchar_t* DebugName)
{
	Capacity       = InCapacity;
	bShaderVisible = bInShaderVisible;

	D3D12_DESCRIPTOR_HEAP_DESC HeapDesc{};
	HeapDesc.Type           = Type;
	HeapDesc.NumDescriptors = Capacity;
	HeapDesc.Flags          = bShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	HeapDesc.NodeMask       = 0;

	E_D3D_VERIFY(Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&Heap)));
	Heap->SetName(DebugName);

	IncrementSize = Device->GetDescriptorHandleIncrementSize(Type);
	CpuStart      = Heap->GetCPUDescriptorHandleForHeapStart();

	// 비가시 힙에서 GPU 핸들을 조회하면 디버그 레이어 오류가 발생하므로 조건부로만 조회
	if (bShaderVisible)
	{
		GpuStart = Heap->GetGPUDescriptorHandleForHeapStart();
	}

	return true;
}

void FD3D12DescriptorHeap::Shutdown()
{
	Heap.Reset();
	CpuStart = {};
	GpuStart = {};
	Capacity = 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE FD3D12DescriptorHeap::GetCpuHandle(uint32 Index) const
{
	E_CHECKF(Index < Capacity, "디스크립터 인덱스 범위 초과: {} / {}", Index, Capacity);

	D3D12_CPU_DESCRIPTOR_HANDLE Handle = CpuStart;
	Handle.ptr += static_cast<SIZE_T>(Index) * IncrementSize;
	return Handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE FD3D12DescriptorHeap::GetGpuHandle(uint32 Index) const
{
	E_CHECKF(bShaderVisible, "셰이더 비가시 힙에서는 GPU 핸들을 사용할 수 없습니다");
	E_CHECKF(Index < Capacity, "디스크립터 인덱스 범위 초과: {} / {}", Index, Capacity);

	D3D12_GPU_DESCRIPTOR_HANDLE Handle = GpuStart;
	Handle.ptr += static_cast<UINT64>(Index) * IncrementSize;
	return Handle;
}

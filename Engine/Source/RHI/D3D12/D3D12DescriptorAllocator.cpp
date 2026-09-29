#include "RHI/D3D12/D3D12DescriptorAllocator.h"

bool FD3D12DescriptorAllocator::Init(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 InCapacity,
                                     bool bInShaderVisible, const wchar_t* DebugName)
{
	Capacity       = InCapacity;
	bShaderVisible = bInShaderVisible;
	NextUnused     = 0;
	FreeList.clear();
	return Heap.Init(Device, Type, Capacity, bShaderVisible, DebugName);
}

void FD3D12DescriptorAllocator::Shutdown()
{
	if (GetAllocatedCount() > 0)
	{
		E_LOG(LogD3D12, Warning, "디스크립터 {}개가 반환되지 않은 채 할당자가 종료됩니다", GetAllocatedCount());
	}
	Heap.Shutdown();
	FreeList.clear();
	NextUnused = 0;
	Capacity   = 0;
}

FD3D12DescriptorHandle FD3D12DescriptorAllocator::Allocate()
{
	uint32 Index;
	if (!FreeList.empty())
	{
		Index = FreeList.back();
		FreeList.pop_back();
	}
	else
	{
		E_CHECKF(NextUnused < Capacity, "디스크립터 힙 용량 초과 ({}개)", Capacity);
		Index = NextUnused++;
	}

	FD3D12DescriptorHandle Handle;
	Handle.Index = Index;
	Handle.Cpu   = Heap.GetCpuHandle(Index);
	if (bShaderVisible)
	{
		Handle.Gpu = Heap.GetGpuHandle(Index);
	}
	return Handle;
}

void FD3D12DescriptorAllocator::FreeByCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle)
{
	const uint32 Index = Heap.GetIndexFromCpuHandle(CpuHandle);
	E_CHECKF(Index != ~0u, "이 할당자의 힙에 속하지 않는 디스크립터입니다");

	FD3D12DescriptorHandle Handle;
	Handle.Index = Index;
	Handle.Cpu   = CpuHandle;
	Free(Handle);
}

void FD3D12DescriptorAllocator::Free(FD3D12DescriptorHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}
	E_CHECKF(Handle.Index < NextUnused, "이 할당자에서 나오지 않은 디스크립터입니다 (인덱스 {})", Handle.Index);

	FreeList.push_back(Handle.Index);
	Handle = FD3D12DescriptorHandle{};
}

#include "RHI/D3D12/D3D12DescriptorAllocator.h"

bool FD3D12DescriptorAllocator::Init(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 InCapacity,
                                     bool bInShaderVisible, const wchar_t* DebugName)
{
	Capacity       = InCapacity;
	bShaderVisible = bInShaderVisible;
	NextUnused     = 0;
	FreeList.clear();
	FreeRanges.clear();
	FreeRangeSlots = 0;
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
	FreeRanges.clear();
	FreeRangeSlots = 0;
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

	return MakeHandle(Index, 1);
}

FD3D12DescriptorHandle FD3D12DescriptorAllocator::MakeHandle(uint32 Index, uint32 Count) const
{
	FD3D12DescriptorHandle Handle;
	Handle.Index = Index;
	Handle.Count = Count;
	Handle.Cpu   = Heap.GetCpuHandle(Index);
	if (bShaderVisible)
	{
		Handle.Gpu = Heap.GetGpuHandle(Index);
	}
	return Handle;
}

FD3D12DescriptorHandle FD3D12DescriptorAllocator::AllocateRange(uint32 Count)
{
	E_CHECKF(Count > 0, "범위 크기는 1 이상이어야 합니다");
	if (Count == 1)
	{
		return Allocate();
	}

	// 같은 크기로 반환된 범위 재사용 (머티리얼 테이블처럼 크기가 고정된 용도가 대부분)
	for (size_t Index = 0; Index < FreeRanges.size(); ++Index)
	{
		if (FreeRanges[Index].Count == Count)
		{
			const uint32 Start = FreeRanges[Index].Start;
			FreeRanges[Index]  = FreeRanges.back();
			FreeRanges.pop_back();
			FreeRangeSlots -= Count;
			return MakeHandle(Start, Count);
		}
	}

	E_CHECKF(NextUnused + Count <= Capacity, "디스크립터 힙 용량 초과 ({}개, 요청 범위 {})", Capacity, Count);
	const uint32 Start = NextUnused;
	NextUnused += Count;
	return MakeHandle(Start, Count);
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
	E_CHECKF(Handle.Index + Handle.Count <= NextUnused, "이 할당자에서 나오지 않은 디스크립터입니다 (인덱스 {})", Handle.Index);

	if (Handle.Count > 1)
	{
		FreeRanges.push_back({ Handle.Index, Handle.Count });
		FreeRangeSlots += Handle.Count;
	}
	else
	{
		FreeList.push_back(Handle.Index);
	}
	Handle = FD3D12DescriptorHandle{};
}

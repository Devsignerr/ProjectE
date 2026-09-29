#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

#include <vector>

struct FD3D12DescriptorHandle
{
	static constexpr uint32 InvalidIndex = ~0u;

	D3D12_CPU_DESCRIPTOR_HANDLE Cpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE Gpu{}; // 셰이더 가시 힙에서만 유효
	uint32                      Index = InvalidIndex;

	bool IsValid() const { return Index != InvalidIndex; }
};

// 프리 리스트 기반 디스크립터 할당자. 하나의 힙에서 디스크립터를 1개 단위로 할당/반환한다.
// 주의: Free는 즉시 재사용 가능 상태로 만들므로 GPU가 해당 디스크립터 사용을 끝낸 뒤 호출해야 한다
// (지연 해제는 Phase 3 리소스 관리자에서 도입).
class FD3D12DescriptorAllocator
{
public:
	bool Init(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32 InCapacity, bool bInShaderVisible,
	          const wchar_t* DebugName);
	void Shutdown();

	FD3D12DescriptorHandle Allocate();
	void                   Free(FD3D12DescriptorHandle& Handle);

	ID3D12DescriptorHeap* GetHeap() const { return Heap.GetHeap(); }
	uint32                GetCapacity() const { return Capacity; }
	uint32                GetAllocatedCount() const { return NextUnused - static_cast<uint32>(FreeList.size()); }

private:
	FD3D12DescriptorHeap Heap;
	std::vector<uint32>  FreeList;
	uint32               NextUnused     = 0;
	uint32               Capacity       = 0;
	bool                 bShaderVisible = false;
};

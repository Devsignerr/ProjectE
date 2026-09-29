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
	uint32                      Count = 1; // 연속 범위 크기 (AllocateRange). Free가 이 값을 보고 반환 방식을 고른다

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
	// 연속 Count칸 (디스크립터 테이블용). 반환 핸들은 첫 칸, Count 필드에 크기
	FD3D12DescriptorHandle AllocateRange(uint32 Count);
	// 단일/범위 모두 반환 (Handle.Count로 구분)
	void                   Free(FD3D12DescriptorHandle& Handle);

	// 범위 내 i번째 칸의 핸들
	D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(uint32 Index) const { return Heap.GetCpuHandle(Index); }
	uint32                      GetIncrementSize() const { return Heap.GetIncrementSize(); }

	// 외부 라이브러리(ImGui 등)가 CPU 핸들만 돌려줄 때 사용
	void FreeByCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle);

	ID3D12DescriptorHeap* GetHeap() const { return Heap.GetHeap(); }
	uint32                GetCapacity() const { return Capacity; }
	uint32                GetAllocatedCount() const { return NextUnused - static_cast<uint32>(FreeList.size()) - FreeRangeSlots; }

private:
	struct FFreeRange
	{
		uint32 Start = 0;
		uint32 Count = 0;
	};

	FD3D12DescriptorHandle MakeHandle(uint32 Index, uint32 Count) const;

	FD3D12DescriptorHeap    Heap;
	std::vector<uint32>     FreeList;       // 단일 칸
	std::vector<FFreeRange> FreeRanges;     // 반환된 범위 (크기가 정확히 같은 요청에 재사용)
	uint32                  FreeRangeSlots = 0;
	uint32               NextUnused     = 0;
	uint32               Capacity       = 0;
	bool                 bShaderVisible = false;
};

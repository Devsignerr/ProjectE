#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <cstring>

struct FD3D12DynamicAllocation
{
	void*                     CpuAddress = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS GpuAddress = 0;
	uint64                    Size       = 0;
};

// 프레임 단위 선형 할당 업로드 버퍼 (상시 매핑).
// 매 프레임 Reset 후 상수 버퍼 등 일회성 데이터를 순차 할당한다. 해당 프레임의 GPU 작업이 끝난 뒤에만 재사용해야 한다.
class FD3D12DynamicUploadBuffer
{
public:
	~FD3D12DynamicUploadBuffer();

	bool Init(ID3D12Device* Device, uint64 InCapacityInBytes, const wchar_t* DebugName);
	void Shutdown();

	// 프레임 시작 시 호출. 이전 할당을 모두 무효화한다.
	void Reset() { Offset = 0; }

	FD3D12DynamicAllocation Allocate(uint64 SizeInBytes, uint64 Alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	// 상수 버퍼용: 256바이트 정렬로 할당하고 Data를 복사한다. 반환된 GpuAddress를 루트 CBV에 바인딩.
	template <typename T>
	FD3D12DynamicAllocation AllocateConstants(const T& Data)
	{
		const FD3D12DynamicAllocation Allocation = Allocate(AlignUp<uint64>(sizeof(T), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
		std::memcpy(Allocation.CpuAddress, &Data, sizeof(T));
		return Allocation;
	}

	uint64 GetCapacity() const { return Capacity; }
	uint64 GetUsed() const { return Offset; }

private:
	ComPtr<ID3D12Resource>    Buffer;
	uint8*                    MappedBase = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS GpuBase    = 0;
	uint64                    Capacity   = 0;
	uint64                    Offset     = 0;
};

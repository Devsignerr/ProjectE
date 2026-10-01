#pragma once

#include "Core/Math/MathUtils.h"
#include "RHI/D3D12/D3D12Common.h"

#include <cstring>
#include <vector>

struct FD3D12DynamicAllocation
{
	void*                     CpuAddress = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS GpuAddress = 0;
	uint64                    Size       = 0;
	ID3D12Resource*           Resource       = nullptr; // 복사 원본으로 쓸 때 (CopyBufferRegion): 할당이 들어 있는 업로드 리소스
	uint64                    ResourceOffset = 0;       // 그 리소스 안 바이트 위치
};

// 프레임 단위 선형 할당 업로드 버퍼 (상시 매핑).
// 매 프레임 Reset 후 상수 버퍼 등 일회성 데이터를 순차 할당한다. 해당 프레임의 GPU 작업이 끝난 뒤에만 재사용해야 한다.
// 용량: 페이지가 차면 같은 프레임 안에서 새 페이지를 덧붙인다 (이미 나간 주소는 그대로 유효 — 할당은 페이지를 넘지 않는다).
//   페이지가 여러 개였던 슬롯은 다음 Reset(= 그 슬롯의 GPU 작업이 끝난 뒤)에 그 프레임 사용량 × 1.25 크기 한 페이지로 합친다.
//   총량이 MaxCapacity를 넘으면 Fatal (무한 증가 방지). 확장할 때마다 경고 로그.
class FD3D12DynamicUploadBuffer
{
public:
	~FD3D12DynamicUploadBuffer();

	bool Init(ID3D12Device* Device, uint64 InCapacityInBytes, const wchar_t* DebugName, uint64 InMaxCapacityInBytes = 256ull * 1024 * 1024);
	void Shutdown();

	// 프레임 시작 시 호출 (그 슬롯의 GPU 작업 완료 후). 이전 할당을 모두 무효화한다.
	void Reset();

	FD3D12DynamicAllocation Allocate(uint64 SizeInBytes, uint64 Alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	// 상수 버퍼용: 256바이트 정렬로 할당하고 Data를 복사한다. 반환된 GpuAddress를 루트 CBV에 바인딩.
	template <typename T>
	FD3D12DynamicAllocation AllocateConstants(const T& Data)
	{
		const FD3D12DynamicAllocation Allocation = Allocate(AlignUp<uint64>(sizeof(T), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
		std::memcpy(Allocation.CpuAddress, &Data, sizeof(T));
		return Allocation;
	}

	uint64 GetCapacity() const { return TotalCapacity; } // 지금 페이지 합계
	uint64 GetMaxCapacity() const { return MaxCapacity; } // 확장 상한
	// 지금 한 번에 할당할 수 있는 최대 크기 (현재 페이지 남은 칸 또는 상한까지 새 페이지) — 많은 데이터를 올리는 기능의 상한 계산용
	uint64 GetMaxAllocation() const
	{
		const uint64 Remaining = Pages.empty() ? 0 : Pages[CurrentPage].Capacity - FMath::Min(Offset, Pages[CurrentPage].Capacity);
		return FMath::Max(Remaining, MaxCapacity > TotalCapacity ? MaxCapacity - TotalCapacity : 0);
	}
	uint64 GetUsed() const { return UsedBeforeCurrent + Offset; } // 이번 프레임 사용량 (페이지 끝 남은 칸 포함)
	uint64 GetLastFrameUsed() const { return LastFrameUsed; } // 이 슬롯의 지난 프레임 사용량 (통계)
	uint32 GetGrowCount() const { return GrowCount; }        // 지금까지 확장한 횟수 (통계)

private:
	struct FPage
	{
		ComPtr<ID3D12Resource>    Buffer;
		uint8*                    Mapped   = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS GpuBase  = 0;
		uint64                    Capacity = 0;
	};
	bool CreatePage(uint64 Bytes, FPage& OutPage);
	void ReleasePages();

	ComPtr<ID3D12Device> Device;
	std::wstring         Name;
	std::vector<FPage>   Pages;
	uint32               CurrentPage       = 0;
	uint64               Offset            = 0; // 현재 페이지 안
	uint64               UsedBeforeCurrent = 0; // 앞 페이지들 사용량
	uint64               TotalCapacity     = 0;
	uint64               MaxCapacity       = 0;
	uint64               LastFrameUsed     = 0;
	uint32               GrowCount         = 0;
};

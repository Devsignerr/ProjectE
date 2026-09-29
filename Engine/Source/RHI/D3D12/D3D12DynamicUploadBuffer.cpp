#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"

FD3D12DynamicUploadBuffer::~FD3D12DynamicUploadBuffer()
{
	Shutdown();
}

bool FD3D12DynamicUploadBuffer::Init(ID3D12Device* Device, uint64 InCapacityInBytes, const wchar_t* DebugName)
{
	E_CHECKF(Buffer == nullptr, "동적 업로드 버퍼가 이미 생성되어 있습니다");

	Capacity = InCapacityInBytes;
	Offset   = 0;

	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC   BufferDesc = MakeBufferDesc(Capacity);
	E_D3D_VERIFY(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
	                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffer)));
	Buffer->SetName(DebugName);

	// CPU는 쓰기만 하므로 읽기 범위 없음. 버퍼 수명 동안 매핑 유지.
	const D3D12_RANGE NoRead{ 0, 0 };
	E_D3D_VERIFY(Buffer->Map(0, &NoRead, reinterpret_cast<void**>(&MappedBase)));

	GpuBase = Buffer->GetGPUVirtualAddress();
	return true;
}

void FD3D12DynamicUploadBuffer::Shutdown()
{
	if (Buffer != nullptr && MappedBase != nullptr)
	{
		Buffer->Unmap(0, nullptr);
	}
	MappedBase = nullptr;
	GpuBase    = 0;
	Capacity   = 0;
	Offset     = 0;
	Buffer.Reset();
}

FD3D12DynamicAllocation FD3D12DynamicUploadBuffer::Allocate(uint64 SizeInBytes, uint64 Alignment)
{
	E_CHECKF(Buffer != nullptr, "동적 업로드 버퍼가 초기화되지 않았습니다");

	const uint64 AlignedOffset = AlignUp(Offset, Alignment);
	E_CHECKF(AlignedOffset + SizeInBytes <= Capacity,
	         "동적 업로드 버퍼 부족: 요청 {} bytes, 사용 {} / {} bytes", SizeInBytes, AlignedOffset, Capacity);

	Offset = AlignedOffset + SizeInBytes;

	FD3D12DynamicAllocation Allocation;
	Allocation.CpuAddress = MappedBase + AlignedOffset;
	Allocation.GpuAddress = GpuBase + AlignedOffset;
	Allocation.Size       = SizeInBytes;
	return Allocation;
}

#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"

#include <format>

FD3D12DynamicUploadBuffer::~FD3D12DynamicUploadBuffer()
{
	Shutdown();
}

bool FD3D12DynamicUploadBuffer::Init(ID3D12Device* InDevice, uint64 InCapacityInBytes, const wchar_t* DebugName, uint64 InMaxCapacityInBytes)
{
	E_CHECKF(Pages.empty(), "동적 업로드 버퍼가 이미 생성되어 있습니다");

	Device      = InDevice;
	Name        = DebugName;
	MaxCapacity = FMath::Max(InMaxCapacityInBytes, InCapacityInBytes);
	Pages.emplace_back();
	if (!CreatePage(InCapacityInBytes, Pages.back()))
	{
		Pages.clear();
		return false;
	}
	TotalCapacity     = InCapacityInBytes;
	CurrentPage       = 0;
	Offset            = 0;
	UsedBeforeCurrent = 0;
	return true;
}

bool FD3D12DynamicUploadBuffer::CreatePage(uint64 Bytes, FPage& OutPage)
{
	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC   BufferDesc = MakeBufferDesc(Bytes);
	E_D3D_VERIFY(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
	                                             IID_PPV_ARGS(&OutPage.Buffer)));
	OutPage.Buffer->SetName(Pages.size() <= 1 ? Name.c_str() : std::format(L"{}_Page{}", Name, Pages.size() - 1).c_str());

	// CPU는 쓰기만 하므로 읽기 범위 없음. 버퍼 수명 동안 매핑 유지.
	const D3D12_RANGE NoRead{ 0, 0 };
	E_D3D_VERIFY(OutPage.Buffer->Map(0, &NoRead, reinterpret_cast<void**>(&OutPage.Mapped)));
	OutPage.GpuBase  = OutPage.Buffer->GetGPUVirtualAddress();
	OutPage.Capacity = Bytes;
	return true;
}

void FD3D12DynamicUploadBuffer::ReleasePages()
{
	for (FPage& Page : Pages)
	{
		if (Page.Buffer != nullptr && Page.Mapped != nullptr)
		{
			Page.Buffer->Unmap(0, nullptr);
		}
	}
	Pages.clear();
	TotalCapacity = 0;
}

void FD3D12DynamicUploadBuffer::Shutdown()
{
	ReleasePages();
	Device.Reset();
	CurrentPage       = 0;
	Offset            = 0;
	UsedBeforeCurrent = 0;
}

void FD3D12DynamicUploadBuffer::Reset()
{
	LastFrameUsed = GetUsed();
	// 지난 프레임에 페이지를 덧붙였으면 사용량에 여유를 더한 한 페이지로 합친다 (이 슬롯의 GPU 작업은 끝났으므로 바로 해제해도 안전)
	if (Pages.size() > 1)
	{
		const uint64 NewCapacity = FMath::Min(AlignUp<uint64>(LastFrameUsed + LastFrameUsed / 4, 1024 * 1024), MaxCapacity);
		ReleasePages();
		Pages.emplace_back();
		if (!CreatePage(NewCapacity, Pages.back()))
		{
			E_LOG(LogD3D12, Fatal, "동적 업로드 버퍼 재생성 실패 ({} bytes)", NewCapacity);
		}
		TotalCapacity = NewCapacity;
		E_LOG(LogD3D12, Log, "동적 업로드 버퍼 합침: {:.1f} MB 한 페이지", static_cast<double>(NewCapacity) / (1024.0 * 1024.0));
	}
	CurrentPage       = 0;
	Offset            = 0;
	UsedBeforeCurrent = 0;
}

FD3D12DynamicAllocation FD3D12DynamicUploadBuffer::Allocate(uint64 SizeInBytes, uint64 Alignment)
{
	E_CHECKF(!Pages.empty(), "동적 업로드 버퍼가 초기화되지 않았습니다");

	uint64 AlignedOffset = AlignUp(Offset, Alignment);
	if (AlignedOffset + SizeInBytes > Pages[CurrentPage].Capacity)
	{
		// 현재 페이지가 모자람: 다음 페이지로 (없으면 만든다)
		UsedBeforeCurrent += Pages[CurrentPage].Capacity;
		++CurrentPage;
		if (CurrentPage >= Pages.size())
		{
			const uint64 PageBytes = FMath::Max(Pages[0].Capacity, AlignUp<uint64>(SizeInBytes, 64 * 1024));
			E_CHECKF(TotalCapacity + PageBytes <= MaxCapacity, "동적 업로드 버퍼 상한 초과: 요청 {} bytes, 사용 {} / 상한 {} bytes", SizeInBytes,
			         UsedBeforeCurrent, MaxCapacity);
			Pages.emplace_back();
			if (!CreatePage(PageBytes, Pages.back()))
			{
				E_LOG(LogD3D12, Fatal, "동적 업로드 버퍼 페이지 생성 실패 ({} bytes)", PageBytes);
			}
			TotalCapacity += PageBytes;
			++GrowCount;
			E_LOG(LogD3D12, Warning, "동적 업로드 버퍼 부족: 페이지 추가 (이번 프레임 {:.1f} MB 사용, 합계 {:.1f} MB)",
			      static_cast<double>(UsedBeforeCurrent) / (1024.0 * 1024.0), static_cast<double>(TotalCapacity) / (1024.0 * 1024.0));
		}
		Offset        = 0;
		AlignedOffset = 0;
	}

	const FPage& Page = Pages[CurrentPage];
	Offset            = AlignedOffset + SizeInBytes;

	FD3D12DynamicAllocation Allocation;
	Allocation.CpuAddress     = Page.Mapped + AlignedOffset;
	Allocation.GpuAddress     = Page.GpuBase + AlignedOffset;
	Allocation.Size           = SizeInBytes;
	Allocation.Resource       = Page.Buffer.Get();
	Allocation.ResourceOffset = AlignedOffset;
	return Allocation;
}

#include "RHI/D3D12/D3D12UploadQueue.h"

#include <algorithm>

FD3D12UploadQueue::~FD3D12UploadQueue()
{
	Shutdown();
}

bool FD3D12UploadQueue::Init(ID3D12Device* InDevice, uint64 RingSize)
{
	E_CHECKF(Device == nullptr, "업로드 큐가 이미 초기화되어 있습니다");
	if (!Queue.Init(InDevice, D3D12_COMMAND_LIST_TYPE_COPY))
	{
		return false;
	}

	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC   RingDesc   = MakeBufferDesc(RingSize);
	E_D3D_VERIFY(InDevice->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &RingDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
	                                               IID_PPV_ARGS(&RingBuffer)));
	RingBuffer->SetName(L"UploadRing");
	const D3D12_RANGE NoRead{ 0, 0 };
	E_D3D_VERIFY(RingBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&RingCpu))); // 영구 매핑 (업로드 힙은 쓰기 결합 — 읽지 않는다)
	Ring.Reset(RingSize);

	Device = InDevice;
	E_LOG(LogD3D12, Display, "업로드 큐 초기화: 복사 큐 + 링 {} MB", RingSize / (1024 * 1024));
	return true;
}

void FD3D12UploadQueue::Shutdown()
{
	if (Device == nullptr)
	{
		return;
	}
	WaitIdle();
	if (bBatchOpen)
	{
		CommandList->Close();
		bBatchOpen = false;
	}
	Open = FBatch{};
	InFlight.clear();
	AwaitingFinalize.clear();
	FreeAllocators.clear();
	CommandList.Reset();
	if (RingBuffer)
	{
		RingBuffer->Unmap(0, nullptr);
	}
	RingBuffer.Reset();
	RingCpu = nullptr;
	Ring.Reset(0);
	Queue.Shutdown();
	Device = nullptr;
}

bool FD3D12UploadQueue::OpenBatch()
{
	if (bBatchOpen)
	{
		return true;
	}
	ComPtr<ID3D12CommandAllocator> Allocator;
	if (!FreeAllocators.empty())
	{
		Allocator = std::move(FreeAllocators.back());
		FreeAllocators.pop_back();
		E_D3D_CHECK(Allocator->Reset());
	}
	else
	{
		E_D3D_CHECK(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&Allocator)));
		Allocator->SetName(L"UploadCopyAllocator");
	}
	if (!CommandList)
	{
		E_D3D_CHECK(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, Allocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));
		CommandList->SetName(L"UploadCopyList");
	}
	else
	{
		E_D3D_CHECK(CommandList->Reset(Allocator.Get(), nullptr));
	}
	Open           = FBatch{};
	Open.Allocator = std::move(Allocator);
	bBatchOpen     = true;
	return true;
}

FD3D12UploadAllocation FD3D12UploadQueue::Allocate(uint64 Size, uint64 Alignment)
{
	E_CHECKF(Device != nullptr, "업로드 큐가 초기화되지 않았습니다");
	UploadedBytes += Size;

	if (Size > Ring.GetCapacity() / 2)
	{
		// 큰 업로드: 전용 스테이징 버퍼 (묶음 완료까지 보관)
		OpenBatch();
		const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
		const D3D12_RESOURCE_DESC   Desc       = MakeBufferDesc(Size);
		ComPtr<ID3D12Resource>      Buffer;
		E_D3D_CHECK(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		                                            IID_PPV_ARGS(&Buffer)));
		Buffer->SetName(L"UploadDedicatedStaging");
		uint8*            Cpu = nullptr;
		const D3D12_RANGE NoRead{ 0, 0 };
		E_D3D_CHECK(Buffer->Map(0, &NoRead, reinterpret_cast<void**>(&Cpu)));
		++DedicatedCount;
		FD3D12UploadAllocation Allocation{ Cpu, Buffer.Get(), 0 };
		Open.Dedicated.push_back(std::move(Buffer));
		return Allocation;
	}

	std::optional<uint64> Offset = Ring.Allocate(Size, Alignment);
	while (!Offset.has_value())
	{
		// 자리 없음: 열린 묶음을 제출하고 끝난 것을 반납, 그래도 없으면 가장 오래된 묶음을 기다린다
		if (bBatchOpen)
		{
			Submit();
		}
		Poll();
		Offset = Ring.Allocate(Size, Alignment);
		if (Offset.has_value())
		{
			break;
		}
		const uint64 Oldest = Ring.GetOldestPendingFence();
		E_CHECKF(Oldest != 0, "업로드 링 할당 실패 ({} bytes)", Size);
		++StallCount;
		E_LOG(LogD3D12, Verbose, "업로드 링 가득 참 — 복사 완료 대기 (펜스 {})", Oldest);
		WaitForFence(Oldest);
		Poll();
		Offset = Ring.Allocate(Size, Alignment);
	}
	OpenBatch(); // 링 할당은 항상 열린 묶음 소유 (Submit에서 그 펜스로 닫힌다)
	return { RingCpu + *Offset, RingBuffer.Get(), *Offset };
}

ID3D12GraphicsCommandList* FD3D12UploadQueue::GetCommandList()
{
	OpenBatch();
	return CommandList.Get();
}

uint64 FD3D12UploadQueue::AddDestination(ID3D12Resource* Resource, bool bTexture)
{
	OpenBatch();
	Open.Destinations.push_back({ Resource, bTexture });
	return Queue.GetNextFenceValue();
}

uint64 FD3D12UploadQueue::Submit()
{
	if (!bBatchOpen)
	{
		return SubmittedFence;
	}
	E_D3D_CHECK(CommandList->Close());
	bBatchOpen      = false;
	Open.FenceValue = Queue.ExecuteCommandList(CommandList.Get());
	SubmittedFence  = Open.FenceValue;
	Ring.CloseBatch(Open.FenceValue);
	InFlight.push_back(std::move(Open));
	Open = FBatch{};
	return SubmittedFence;
}

void FD3D12UploadQueue::Poll()
{
	if (Device == nullptr)
	{
		return;
	}
	const uint64 Completed = Queue.GetCompletedFenceValue();
	while (!InFlight.empty() && InFlight.front().FenceValue <= Completed)
	{
		FBatch& Batch = InFlight.front();
		FreeAllocators.push_back(std::move(Batch.Allocator));
		for (FDestination& Destination : Batch.Destinations)
		{
			AwaitingFinalize.push_back(std::move(Destination));
		}
		InFlight.pop_front();
	}
	Ring.Retire(Completed);
	PolledFence = std::max(PolledFence, std::min(Completed, SubmittedFence));
}

void FD3D12UploadQueue::WaitForFence(uint64 FenceValue)
{
	Queue.WaitForFenceValue(FenceValue);
}

void FD3D12UploadQueue::WaitIdle()
{
	if (Device == nullptr)
	{
		return;
	}
	Submit();
	WaitForFence(SubmittedFence);
	Poll();
}

void FD3D12UploadQueue::RecordFinalize(ID3D12GraphicsCommandList* DirectList, std::vector<ComPtr<ID3D12Resource>>& OutKeepAlive)
{
	std::vector<D3D12_RESOURCE_BARRIER> Barriers;
	for (FDestination& Destination : AwaitingFinalize)
	{
		if (Destination.bTexture)
		{
			Barriers.push_back(MakeTransitionBarrier(Destination.Resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		}
	}
	if (!Barriers.empty())
	{
		DirectList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
	}
	for (FDestination& Destination : AwaitingFinalize)
	{
		OutKeepAlive.push_back(std::move(Destination.Resource));
	}
	AwaitingFinalize.clear();
	FinalizedFence = PolledFence;
}

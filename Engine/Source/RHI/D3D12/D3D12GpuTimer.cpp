#include "RHI/D3D12/D3D12GpuTimer.h"

#include <algorithm>

FD3D12GpuTimer::~FD3D12GpuTimer()
{
	Shutdown();
}

bool FD3D12GpuTimer::Init(ID3D12Device* Device, ID3D12CommandQueue* Queue, uint32 InSlotCount, const wchar_t* DebugName)
{
	SlotCount = std::clamp<uint32>(InSlotCount, 1, MaxSlots);

	uint64 Frequency = 0;
	if (FAILED(Queue->GetTimestampFrequency(&Frequency)) || Frequency == 0)
	{
		E_LOG(LogD3D12, Warning, "타임스탬프 주파수를 얻지 못해 GPU 시간을 측정하지 않습니다");
		return false;
	}
	MsPerTick = 1000.0 / static_cast<double>(Frequency);

	const uint32          QueryCount = SlotCount * MaxScopes * 2;
	D3D12_QUERY_HEAP_DESC HeapDesc{};
	HeapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	HeapDesc.Count = QueryCount;
	E_D3D_VERIFY(Device->CreateQueryHeap(&HeapDesc, IID_PPV_ARGS(&QueryHeap)));
	QueryHeap->SetName(DebugName);

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);
	const D3D12_RESOURCE_DESC   Desc = MakeBufferDesc(sizeof(uint64) * QueryCount);
	E_D3D_VERIFY(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                             IID_PPV_ARGS(&Readback)));
	Readback->SetName(DebugName);
	return true;
}

void FD3D12GpuTimer::Shutdown()
{
	// 호출자가 GPU Flush 이후 부른다
	QueryHeap.Reset();
	Readback.Reset();
	bRecording  = false;
	bHasResults = false;
	std::fill(std::begin(SlotScopeMask), std::end(SlotScopeMask), 0u);
}

bool FD3D12GpuTimer::BeginFrame(uint32 Slot, uint64 FrameNumber)
{
	bRecording = false;
	if (!QueryHeap || FrameNumber == LastFrame)
	{
		return false;
	}
	LastFrame     = FrameNumber;
	CurrentSlot   = Slot % SlotCount;
	OpenScopeMask = 0;
	ReadSlot(CurrentSlot);
	SlotScopeMask[CurrentSlot] = 0;
	bRecording                 = true;
	return true;
}

void FD3D12GpuTimer::ReadSlot(uint32 Slot)
{
	const uint32 Mask = SlotScopeMask[Slot];
	if (Mask == 0)
	{
		return;
	}
	const uint64 Base = static_cast<uint64>(Slot) * MaxScopes * 2;
	const D3D12_RANGE ReadRange{ Base * sizeof(uint64), (Base + MaxScopes * 2) * sizeof(uint64) };
	uint64*           Data = nullptr;
	if (FAILED(Readback->Map(0, &ReadRange, reinterpret_cast<void**>(&Data))))
	{
		return;
	}
	for (uint32 Scope = 0; Scope < MaxScopes; ++Scope)
	{
		if ((Mask & (1u << Scope)) == 0)
		{
			ResultsMs[Scope] = 0.0f;
			continue;
		}
		const uint64 Begin = Data[Base + Scope * 2];
		const uint64 End   = Data[Base + Scope * 2 + 1];
		ResultsMs[Scope]   = End > Begin ? static_cast<float>(static_cast<double>(End - Begin) * MsPerTick) : 0.0f;
	}
	const D3D12_RANGE NoWrite{ 0, 0 };
	Readback->Unmap(0, &NoWrite);
	bHasResults = true;
}

void FD3D12GpuTimer::BeginScope(ID3D12GraphicsCommandList* CommandList, uint32 Scope)
{
	if (!bRecording || Scope >= MaxScopes)
	{
		return;
	}
	CommandList->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (CurrentSlot * MaxScopes + Scope) * 2);
	OpenScopeMask |= 1u << Scope;
}

void FD3D12GpuTimer::EndScope(ID3D12GraphicsCommandList* CommandList, uint32 Scope)
{
	if (!bRecording || Scope >= MaxScopes || (OpenScopeMask & (1u << Scope)) == 0)
	{
		return;
	}
	CommandList->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (CurrentSlot * MaxScopes + Scope) * 2 + 1);
	OpenScopeMask &= ~(1u << Scope);
	SlotScopeMask[CurrentSlot] |= 1u << Scope;
}

void FD3D12GpuTimer::EndFrame(ID3D12GraphicsCommandList* CommandList)
{
	if (!bRecording)
	{
		return;
	}
	bRecording = false;
	// 끝낸 구간만 옮긴다 (쓰지 않은 쿼리를 옮기면 디버그 레이어 경고)
	const uint32 Mask = SlotScopeMask[CurrentSlot];
	for (uint32 Scope = 0; Scope < MaxScopes; ++Scope)
	{
		if ((Mask & (1u << Scope)) != 0)
		{
			const uint32 Index = (CurrentSlot * MaxScopes + Scope) * 2;
			CommandList->ResolveQueryData(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Index, 2, Readback.Get(), sizeof(uint64) * Index);
		}
	}
}

#include "RHI/D3D12/D3D12CommandQueue.h"

FD3D12CommandQueue::~FD3D12CommandQueue()
{
	Shutdown();
}

bool FD3D12CommandQueue::Init(ID3D12Device* Device, D3D12_COMMAND_LIST_TYPE InType, D3D12_COMMAND_QUEUE_PRIORITY Priority)
{
	Type = InType;

	D3D12_COMMAND_QUEUE_DESC QueueDesc{};
	QueueDesc.Type     = Type;
	QueueDesc.Priority = Priority;
	QueueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
	QueueDesc.NodeMask = 0;

	E_D3D_VERIFY(Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(&Queue)));
	E_D3D_VERIFY(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));

	switch (Type)
	{
	case D3D12_COMMAND_LIST_TYPE_COMPUTE: Queue->SetName(L"ComputeQueue"); Fence->SetName(L"ComputeQueueFence"); break;
	case D3D12_COMMAND_LIST_TYPE_COPY:    Queue->SetName(L"CopyQueue");    Fence->SetName(L"CopyQueueFence");    break;
	default:                              Queue->SetName(L"DirectQueue");  Fence->SetName(L"DirectQueueFence");  break;
	}

	FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (FenceEvent == nullptr)
	{
		E_LOG(LogD3D12, Error, "펜스 이벤트 생성 실패 (오류 코드 {})", GetLastError());
		return false;
	}

	return true;
}

void FD3D12CommandQueue::Shutdown()
{
	if (Queue)
	{
		Flush();
	}

	if (FenceEvent != nullptr)
	{
		CloseHandle(FenceEvent);
		FenceEvent = nullptr;
	}

	Fence.Reset();
	Queue.Reset();
}

uint64 FD3D12CommandQueue::ExecuteCommandList(ID3D12CommandList* CommandList)
{
	ID3D12CommandList* Lists[] = { CommandList };
	Queue->ExecuteCommandLists(1, Lists);
	return Signal();
}

bool FD3D12CommandQueue::ExecuteImmediate(ID3D12Device* Device, const std::function<void(ID3D12GraphicsCommandList*)>& Record)
{
	ComPtr<ID3D12CommandAllocator>    Allocator;
	ComPtr<ID3D12GraphicsCommandList> CommandList;
	E_D3D_VERIFY(Device->CreateCommandAllocator(Type, IID_PPV_ARGS(&Allocator)));
	E_D3D_VERIFY(Device->CreateCommandList(0, Type, Allocator.Get(), nullptr, IID_PPV_ARGS(&CommandList)));
	CommandList->SetName(L"ImmediateCommandList");

	Record(CommandList.Get());

	E_D3D_VERIFY(CommandList->Close());
	WaitForFenceValue(ExecuteCommandList(CommandList.Get()));
	return true;
}

uint64 FD3D12CommandQueue::Signal()
{
	const uint64 FenceValue = NextFenceValue++;
	E_D3D_CHECK(Queue->Signal(Fence.Get(), FenceValue));
	return FenceValue;
}

bool FD3D12CommandQueue::IsFenceComplete(uint64 FenceValue) const
{
	return Fence->GetCompletedValue() >= FenceValue;
}

void FD3D12CommandQueue::WaitForFenceValue(uint64 FenceValue)
{
	if (IsFenceComplete(FenceValue))
	{
		return;
	}

	E_D3D_CHECK(Fence->SetEventOnCompletion(FenceValue, FenceEvent));
	WaitForSingleObject(FenceEvent, INFINITE);
}

void FD3D12CommandQueue::Flush()
{
	WaitForFenceValue(Signal());
}

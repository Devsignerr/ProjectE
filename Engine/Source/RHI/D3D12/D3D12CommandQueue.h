#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <functional>

// 커맨드 큐 + 펜스 기반 CPU/GPU 동기화
class FD3D12CommandQueue
{
public:
	~FD3D12CommandQueue();

	bool Init(ID3D12Device* Device, D3D12_COMMAND_LIST_TYPE InType);
	void Shutdown();

	// 커맨드 리스트 실행 후 Signal. 완료 대기에 사용할 펜스 값을 반환.
	uint64 ExecuteCommandList(ID3D12CommandList* CommandList);

	// 임시 커맨드 리스트를 만들어 Record를 기록·실행하고 완료까지 동기 대기 (리소스 업로드 등 로딩 시점용)
	bool ExecuteImmediate(ID3D12Device* Device, const std::function<void(ID3D12GraphicsCommandList*)>& Record);

	// 큐에 펜스 신호를 기록하고 그 값을 반환
	uint64 Signal();

	bool IsFenceComplete(uint64 FenceValue) const;
	void WaitForFenceValue(uint64 FenceValue);

	// 큐에 제출된 모든 작업이 끝날 때까지 대기
	void Flush();

	ID3D12CommandQueue*     GetQueue() const { return Queue.Get(); }
	D3D12_COMMAND_LIST_TYPE GetType() const { return Type; }

private:
	ComPtr<ID3D12CommandQueue> Queue;
	ComPtr<ID3D12Fence>        Fence;
	HANDLE                     FenceEvent     = nullptr;
	uint64                     NextFenceValue = 1;
	D3D12_COMMAND_LIST_TYPE    Type           = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

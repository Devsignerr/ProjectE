#pragma once

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Common.h"
#include "RHI/UploadRingAllocator.h"

#include <deque>
#include <vector>

// 스테이징 메모리 한 덩어리 (링 또는 전용 업로드 버퍼). 묶음이 GPU에서 끝날 때까지 유효
struct FD3D12UploadAllocation
{
	uint8*          Cpu      = nullptr;
	ID3D12Resource* Resource = nullptr; // 업로드 힙 버퍼 (복사 원본)
	uint64          Offset   = 0;       // Resource 안 오프셋
};

// 비동기 업로드: 복사 큐(COPY) + 영구 매핑 업로드 링. 정적 버퍼/텍스처 초기 데이터를 CPU 대기 없이 GPU로 보낸다.
//   흐름: Allocate(스테이징) → GetCommandList에 복사 기록 → AddDestination(대상 등록, 반환 = 이 묶음의 펜스)
//         → Submit(묶음 실행 + 신호: RHI가 BeginFrame/EndFrame에 부른다) → Poll(완료 펜스 → 링/할당자 반납)
//         → RecordFinalize(직접 큐 명령 리스트에 텍스처 전이 기록 → FinalizedFence 갱신).
//   리소스 상태 규칙 (D3D12 복사 큐):
//     - 대상은 COMMON으로 만든다. 복사 큐에서 COPY_DEST로 암시적 승격, ExecuteCommandLists가 끝나면 COMMON으로 감쇠
//     - 텍스처는 완료 후 직접 큐에서 COMMON → PIXEL_SHADER_RESOURCE로 명시 전이한다 (기존 텍스처와 같은 상태 불변식)
//     - 버퍼는 전이하지 않는다 (직접 큐에서 VB/IB 등으로 암시적 승격 — 기존 정적 버퍼와 같음)
//   준비 판정: 대상의 펜스 <= GetFinalizedFence()면 사용 가능 (전이가 그 프레임 명령 리스트 맨 앞에 기록됨)
//   링이 가득 차면 열린 묶음을 제출하고 가장 오래된 묶음을 CPU로 기다린다 (드묾 — GetStallCount),
//   링 절반보다 큰 업로드는 전용 업로드 버퍼(묶음 완료까지 보관)를 쓴다.
//   메인 스레드 전용.
class FD3D12UploadQueue
{
public:
	static constexpr uint64 DefaultRingSize         = 64ull * 1024 * 1024;
	static constexpr uint64 TexturePlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // 512

	~FD3D12UploadQueue();

	bool Init(ID3D12Device* InDevice, uint64 RingSize = DefaultRingSize);
	// 모든 복사가 끝나기를 기다린 뒤 해제 (전이 대기 중인 대상 참조도 버린다 — 장치 종료 직전)
	void Shutdown();
	bool IsInitialized() const { return Device != nullptr; }

	// 스테이징 메모리. 링이 가득 차면 열린 묶음을 제출할 수 있으므로 복사 기록 전에 먼저 할당한다
	FD3D12UploadAllocation Allocate(uint64 Size, uint64 Alignment);
	// 열린 묶음의 복사 명령 리스트 (없으면 연다)
	ID3D12GraphicsCommandList* GetCommandList();
	// 복사 대상 등록: 묶음 완료까지 참조를 쥐고, bTexture면 완료 후 직접 큐 전이 대상. 반환 = 이 묶음이 신호할 펜스
	uint64 AddDestination(ID3D12Resource* Resource, bool bTexture);

	// 열린 묶음을 닫아 실행 + 신호. 반환 = 마지막으로 제출한 펜스 (열린 묶음이 없으면 이전 값)
	uint64 Submit();
	// 완료 펜스를 읽고 끝난 묶음의 링 공간/할당자/전용 버퍼를 반납 (완료 대상은 전이 대기 목록으로)
	void Poll();
	void WaitForFence(uint64 FenceValue);
	// 제출 + 마지막 펜스까지 CPU 대기 + Poll
	void WaitIdle();

	// 완료된 텍스처의 COMMON → PIXEL_SHADER_RESOURCE 전이를 DirectList에 기록하고 FinalizedFence를 Poll 시점 완료 값으로 올린다.
	// OutKeepAlive: 기록한 대상 참조 (그 명령 리스트 실행이 끝날 때까지 살려야 함 — RHI가 지연 해제로 넘긴다)
	void RecordFinalize(ID3D12GraphicsCommandList* DirectList, std::vector<ComPtr<ID3D12Resource>>& OutKeepAlive);
	bool HasPendingFinalize() const { return !AwaitingFinalize.empty() || FinalizedFence < PolledFence; }

	uint64 GetFinalizedFence() const { return FinalizedFence; }
	uint64 GetCompletedFence() const { return PolledFence; }
	uint64 GetSubmittedFence() const { return SubmittedFence; }
	ID3D12Fence*        GetFence() const { return Queue.GetFence(); }
	FD3D12CommandQueue& GetQueue() { return Queue; }

	// 통계
	uint64 GetRingCapacity() const { return Ring.GetCapacity(); }
	uint64 GetRingUsedBytes() const { return Ring.GetUsedBytes(); }
	uint64 GetStallCount() const { return StallCount; }           // 링이 가득 차 CPU가 기다린 횟수
	uint64 GetDedicatedCount() const { return DedicatedCount; }   // 링보다 커서 전용 버퍼를 쓴 횟수
	uint64 GetUploadedBytes() const { return UploadedBytes; }     // 누적 스테이징 바이트
	size_t GetInFlightBatchCount() const { return InFlight.size(); }

private:
	struct FDestination
	{
		ComPtr<ID3D12Resource> Resource;
		bool                   bTexture = false;
	};
	struct FBatch
	{
		uint64                              FenceValue = 0;
		ComPtr<ID3D12CommandAllocator>      Allocator;
		std::vector<ComPtr<ID3D12Resource>> Dedicated;
		std::vector<FDestination>           Destinations;
	};

	bool OpenBatch();

	ID3D12Device*                     Device = nullptr; // 소유하지 않음 (FD3D12Device 수명)
	FD3D12CommandQueue                Queue;
	ComPtr<ID3D12GraphicsCommandList> CommandList;
	ComPtr<ID3D12Resource>            RingBuffer;
	uint8*                            RingCpu = nullptr;
	FUploadRingAllocator              Ring;

	bool                                        bBatchOpen = false;
	FBatch                                      Open;
	std::deque<FBatch>                          InFlight;
	std::vector<ComPtr<ID3D12CommandAllocator>> FreeAllocators;
	std::vector<FDestination>                   AwaitingFinalize;

	uint64 SubmittedFence = 0;
	uint64 PolledFence    = 0;
	uint64 FinalizedFence = 0;
	uint64 StallCount     = 0;
	uint64 DedicatedCount = 0;
	uint64 UploadedBytes  = 0;
};

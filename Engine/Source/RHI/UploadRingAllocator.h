#pragma once

#include "Core/CoreTypes.h"

#include <deque>
#include <optional>

// 업로드 링 할당 규칙 (순수 로직 — GPU 없음, RhiTests가 검증). FD3D12UploadQueue가 영구 매핑 업로드 버퍼 위에서 쓴다.
//   - [0, Capacity) 바이트를 앞에서부터 차례로 나눠 준다. 끝에 안 맞으면 남은 꼬리를 버리고 0으로 돌아간다 (버린 바이트도 그 묶음 소유)
//   - 열린 묶음의 할당은 CloseBatch(펜스)로 그 펜스 값에 묶이고, Retire(완료 펜스)가 완료된 묶음을 오래된 순서대로 돌려준다
//   - 묶음은 펜스 값이 증가하는 순서로 닫아야 한다 (복사 큐 하나가 신호하는 값)
//   - 모두 반납되면(사용량 0) 머리/꼬리를 0으로 되돌려 다음 큰 할당이 끊기지 않게 한다
class FUploadRingAllocator
{
public:
	explicit FUploadRingAllocator(uint64 InCapacity = 0) { Reset(InCapacity); }

	void Reset(uint64 InCapacity);

	// Alignment는 2의 거듭제곱. 자리가 없으면 nullopt (호출자가 묶음을 제출하고 완료를 기다린 뒤 다시 시도)
	std::optional<uint64> Allocate(uint64 Size, uint64 Alignment);

	// 지금까지 열린 묶음의 할당을 FenceValue에 묶는다 (열린 할당이 없으면 아무 일도 없음)
	void CloseBatch(uint64 FenceValue);

	// FenceValue <= CompletedFence인 묶음을 반납
	void Retire(uint64 CompletedFence);

	uint64 GetCapacity() const { return Capacity; }
	uint64 GetUsedBytes() const { return UsedBytes; } // 열린 묶음 + 완료 대기 묶음 (정렬 여백·버린 꼬리 포함)
	uint64 GetOpenBytes() const { return OpenBytes; }
	size_t GetPendingBatchCount() const { return Batches.size(); }
	// 가장 오래된 완료 대기 묶음의 펜스 (없으면 0) — 자리가 없을 때 이 값을 기다리면 된다
	uint64 GetOldestPendingFence() const { return Batches.empty() ? 0 : Batches.front().FenceValue; }

private:
	struct FBatch
	{
		uint64 FenceValue = 0;
		uint64 EndOffset  = 0; // 이 묶음이 끝난 뒤의 머리 위치 (반납하면 꼬리가 여기로)
		uint64 Bytes      = 0;
	};

	uint64             Capacity  = 0;
	uint64             Head      = 0; // 다음 할당 위치
	uint64             Tail      = 0; // 가장 오래된 사용 중 바이트
	uint64             UsedBytes = 0;
	uint64             OpenBytes = 0;
	std::deque<FBatch> Batches;
};

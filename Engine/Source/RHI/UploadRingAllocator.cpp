#include "RHI/UploadRingAllocator.h"

void FUploadRingAllocator::Reset(uint64 InCapacity)
{
	Capacity  = InCapacity;
	Head      = 0;
	Tail      = 0;
	UsedBytes = 0;
	OpenBytes = 0;
	Batches.clear();
}

std::optional<uint64> FUploadRingAllocator::Allocate(uint64 Size, uint64 Alignment)
{
	if (Size == 0 || Size > Capacity || Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
	{
		return std::nullopt;
	}
	if (UsedBytes == 0)
	{
		Head = 0;
		Tail = 0;
	}

	const auto Take = [&](uint64 Offset, uint64 Consumed) {
		Head = Offset + Size;
		UsedBytes += Consumed;
		OpenBytes += Consumed;
		return std::optional<uint64>(Offset);
	};

	const uint64 Aligned = (Head + Alignment - 1) & ~(Alignment - 1);
	if (UsedBytes == 0 || Head > Tail)
	{
		// 사용 중 구간 [Tail, Head) — 빈 곳은 [Head, Capacity)와 [0, Tail)
		if (Aligned + Size <= Capacity)
		{
			return Take(Aligned, Aligned + Size - Head);
		}
		if (UsedBytes > 0 && Size <= Tail)
		{
			const uint64 Wasted = Capacity - Head; // 끝에 남은 꼬리는 버리고 0으로
			return Take(0, Wasted + Size);
		}
		return std::nullopt;
	}
	if (Head < Tail && Aligned + Size <= Tail)
	{
		// 한 바퀴 돈 상태 — 빈 곳은 [Head, Tail)
		return Take(Aligned, Aligned + Size - Head);
	}
	return std::nullopt; // Head == Tail (가득 참) 또는 자리 부족
}

void FUploadRingAllocator::CloseBatch(uint64 FenceValue)
{
	if (OpenBytes == 0)
	{
		return;
	}
	Batches.push_back({ FenceValue, Head, OpenBytes });
	OpenBytes = 0;
}

void FUploadRingAllocator::Retire(uint64 CompletedFence)
{
	while (!Batches.empty() && Batches.front().FenceValue <= CompletedFence)
	{
		Tail = Batches.front().EndOffset;
		UsedBytes -= Batches.front().Bytes;
		Batches.pop_front();
	}
	if (UsedBytes == 0)
	{
		Head = 0;
		Tail = 0;
	}
}

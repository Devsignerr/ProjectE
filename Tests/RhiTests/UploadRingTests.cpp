#include "Core/Testing/TestFramework.h"
#include "RHI/UploadRingAllocator.h"

// FUploadRingAllocator: 순수 할당 규칙 (GPU 없음)

E_TEST(UploadRing_AllocatesSequentiallyWithAlignment)
{
	FUploadRingAllocator Ring(1024);
	const std::optional<uint64> A = Ring.Allocate(10, 16);
	const std::optional<uint64> B = Ring.Allocate(10, 16);
	const std::optional<uint64> C = Ring.Allocate(100, 512);
	E_EXPECT_TRUE(A.has_value() && B.has_value() && C.has_value());
	E_EXPECT_EQ(*A, 0ull);
	E_EXPECT_EQ(*B, 16ull);
	E_EXPECT_EQ(*C, 512ull);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 612ull); // 정렬 여백 포함
	E_EXPECT_EQ(Ring.GetOpenBytes(), 612ull);
}

E_TEST(UploadRing_RejectsInvalidAndFull)
{
	FUploadRingAllocator Ring(256);
	E_EXPECT_FALSE(Ring.Allocate(0, 16).has_value());
	E_EXPECT_FALSE(Ring.Allocate(257, 16).has_value());
	E_EXPECT_FALSE(Ring.Allocate(16, 3).has_value()); // 2의 거듭제곱 아님
	E_EXPECT_TRUE(Ring.Allocate(256, 16).has_value());
	E_EXPECT_FALSE(Ring.Allocate(1, 1).has_value()); // 가득 참
	Ring.CloseBatch(1);
	E_EXPECT_FALSE(Ring.Allocate(1, 1).has_value()); // 펜스 완료 전에는 계속 가득 참
	Ring.Retire(1);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 0ull);
	const std::optional<uint64> Again = Ring.Allocate(256, 16);
	E_EXPECT_TRUE(Again.has_value() && *Again == 0ull); // 모두 반납되면 0부터
}

E_TEST(UploadRing_RetiresInFenceOrderOnly)
{
	FUploadRingAllocator Ring(1000);
	E_EXPECT_TRUE(Ring.Allocate(300, 1).has_value());
	Ring.CloseBatch(5);
	E_EXPECT_TRUE(Ring.Allocate(300, 1).has_value());
	Ring.CloseBatch(6);
	E_EXPECT_EQ(Ring.GetPendingBatchCount(), size_t(2));
	E_EXPECT_EQ(Ring.GetOldestPendingFence(), 5ull);

	Ring.Retire(4);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 600ull);
	Ring.Retire(5);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 300ull);
	E_EXPECT_EQ(Ring.GetOldestPendingFence(), 6ull);
	Ring.Retire(100);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 0ull);
	E_EXPECT_EQ(Ring.GetOldestPendingFence(), 0ull);
}

E_TEST(UploadRing_WrapsAroundAndCountsWastedTail)
{
	FUploadRingAllocator Ring(1000);
	E_EXPECT_TRUE(Ring.Allocate(400, 1).has_value()); // [0,400)
	Ring.CloseBatch(1);
	E_EXPECT_TRUE(Ring.Allocate(400, 1).has_value()); // [400,800)
	Ring.CloseBatch(2);
	Ring.Retire(1); // 꼬리 = 400

	// 끝에 200바이트 남음 → 300은 안 맞으니 꼬리를 버리고 0으로 (0~400이 비었다)
	const std::optional<uint64> Wrapped = Ring.Allocate(300, 1);
	E_EXPECT_TRUE(Wrapped.has_value() && *Wrapped == 0ull);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 400ull + 200ull + 300ull); // 묶음 2 + 버린 꼬리 + 새 할당
	// 감긴 상태: 빈 곳은 [300, 400)
	E_EXPECT_FALSE(Ring.Allocate(101, 1).has_value());
	const std::optional<uint64> Fits = Ring.Allocate(100, 1);
	E_EXPECT_TRUE(Fits.has_value() && *Fits == 300ull);
	Ring.CloseBatch(3);

	Ring.Retire(2); // 꼬리 = 800 (묶음 2 끝)
	E_EXPECT_EQ(Ring.GetUsedBytes(), 600ull);
	Ring.Retire(3);
	E_EXPECT_EQ(Ring.GetUsedBytes(), 0ull);
}

E_TEST(UploadRing_ManyBatchesNeverOverlap)
{
	// 무작위에 가까운 크기로 여러 번 돌면서, 살아 있는 구간끼리 겹치지 않는지
	FUploadRingAllocator Ring(4096);
	struct FLive
	{
		uint64 Offset;
		uint64 Size;
		uint64 Fence;
	};
	std::vector<FLive> Live;
	uint64             Fence     = 0;
	uint64             Completed = 0;
	uint32             Seed      = 12345;
	bool               bOverlap  = false;
	for (int32 Step = 0; Step < 2000; ++Step)
	{
		Seed              = Seed * 1664525u + 1013904223u;
		const uint64 Size = 1 + (Seed >> 8) % 700;
		const uint64 Align = 1ull << ((Seed >> 4) % 10);
		std::optional<uint64> Offset = Ring.Allocate(Size, Align);
		while (!Offset.has_value())
		{
			Ring.CloseBatch(++Fence);
			for (FLive& Entry : Live)
			{
				if (Entry.Fence == 0)
				{
					Entry.Fence = Fence;
				}
			}
			++Completed;
			Ring.Retire(Completed);
			std::erase_if(Live, [&](const FLive& Entry) { return Entry.Fence != 0 && Entry.Fence <= Completed; });
			Offset = Ring.Allocate(Size, Align);
		}
		E_EXPECT_TRUE(*Offset % Align == 0 || *Offset == 0);
		E_EXPECT_TRUE(*Offset + Size <= 4096);
		for (const FLive& Entry : Live)
		{
			bOverlap |= *Offset < Entry.Offset + Entry.Size && Entry.Offset < *Offset + Size;
		}
		Live.push_back({ *Offset, Size, 0 });
		if (Step % 3 == 0)
		{
			Ring.CloseBatch(++Fence);
			for (FLive& Entry : Live)
			{
				if (Entry.Fence == 0)
				{
					Entry.Fence = Fence;
				}
			}
		}
	}
	E_EXPECT_FALSE(bOverlap);
}

#include "Renderer/InstanceBatching.h"

#include "Core/Jobs/ParallelFor.h"

#include <algorithm>

namespace InstanceBatching
{
	namespace
	{
		// 기준 순서: 키 → 깊이 → 인스턴스 번호 (같은 깊이면 번호 순 — 결과가 실행마다 같도록)
		bool LessFrontToBack(const FInstanceSortItem& A, const FInstanceSortItem& B)
		{
			if (A.Key != B.Key)
			{
				return A.Key < B.Key;
			}
			if (A.Depth != B.Depth)
			{
				return A.Depth < B.Depth;
			}
			return A.Instance < B.Instance;
		}

		// 정렬된 항목을 이어 붙이며 바로 앞과 키가 같으면 같은 묶음으로
		void MergeSorted(const std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches)
		{
			OutIndices.reserve(Items.size());
			for (size_t Index = 0; Index < Items.size(); ++Index)
			{
				const FInstanceSortItem& Item = Items[Index];
				const bool bContinue = !OutBatches.empty() && !IsUniqueKey(Item.Key) && Items[Index - 1].Key == Item.Key;
				if (bContinue)
				{
					++OutBatches.back().Count;
				}
				else
				{
					OutBatches.push_back({ static_cast<uint32>(OutIndices.size()), 1u, Item.Instance, Item.Key });
				}
				OutIndices.push_back(Item.Instance);
			}
		}

		// 스레드별 임시 버퍼 (그림자 캐스케이드처럼 여러 스레드가 동시에 정렬한다). 프레임마다 다시 할당하지 않게 유지
		struct FSortScratch
		{
			std::vector<FInstanceSortItem> Sorted;
			std::vector<uint64>            TableKeys;  // 열린 주소 해시: 키
			std::vector<uint32>            TableSlots; // 열린 주소 해시: 고유 키 번호 (EmptySlot = 빈 칸)
			std::vector<uint64>            Distinct;   // 고유 키 (처음 나온 순)
			std::vector<uint32>            SlotOfItem; // 항목 → 고유 키 번호
			std::vector<uint32>            Order;      // 고유 키 번호를 키 순으로
			std::vector<uint32>            GroupStart; // 고유 키 번호 → 정렬 결과 안 시작 위치 (분배 중에는 다음 쓸 위치)
			std::vector<uint32>            GroupBegin; // 키 순위 → 정렬 결과 안 시작 위치 (+ 끝 하나)
		};
		thread_local FSortScratch SortScratch;

		constexpr uint32 EmptySlot = 0xFFFFFFFFu;

		uint32 HashIndex(uint64 Key, uint32 Mask)
		{
			return static_cast<uint32>((Key * 0x9E3779B97F4A7C15ull) >> 32) & Mask;
		}

		void ResetTable(FSortScratch& Scratch, uint32 Capacity)
		{
			Scratch.TableKeys.assign(Capacity, 0);
			Scratch.TableSlots.assign(Capacity, EmptySlot);
		}

		void InsertTable(FSortScratch& Scratch, uint64 Key, uint32 Slot)
		{
			const uint32 Mask  = static_cast<uint32>(Scratch.TableSlots.size()) - 1;
			uint32       Index = HashIndex(Key, Mask);
			while (Scratch.TableSlots[Index] != EmptySlot)
			{
				Index = (Index + 1) & Mask;
			}
			Scratch.TableKeys[Index]  = Key;
			Scratch.TableSlots[Index] = Slot;
		}

		// 키의 고유 번호 (없으면 새로)
		uint32 FindOrAddKey(FSortScratch& Scratch, uint64 Key)
		{
			const uint32 Mask  = static_cast<uint32>(Scratch.TableSlots.size()) - 1;
			uint32       Index = HashIndex(Key, Mask);
			while (Scratch.TableSlots[Index] != EmptySlot)
			{
				if (Scratch.TableKeys[Index] == Key)
				{
					return Scratch.TableSlots[Index];
				}
				Index = (Index + 1) & Mask;
			}
			const uint32 Slot = static_cast<uint32>(Scratch.Distinct.size());
			Scratch.Distinct.push_back(Key);
			if (Scratch.Distinct.size() * 2 > Scratch.TableSlots.size())
			{
				// 채움률 1/2를 넘으면 두 배로 다시 넣는다
				ResetTable(Scratch, static_cast<uint32>(Scratch.TableSlots.size()) * 2);
				for (uint32 Existing = 0; Existing < static_cast<uint32>(Scratch.Distinct.size()); ++Existing)
				{
					InsertTable(Scratch, Scratch.Distinct[Existing], Existing);
				}
			}
			else
			{
				Scratch.TableKeys[Index]  = Key;
				Scratch.TableSlots[Index] = Slot;
			}
			return Slot;
		}

		// 키 묶음 정렬: 결과는 LessFrontToBack 정렬과 같다. 조건 = 입력이 인스턴스 번호 오름차순 + NaN 깊이 없음
		// (안 맞으면 false — 부른 쪽이 비교 정렬). 고유 키만 비교 정렬해 순위를 매기고 키별로 안정 분배(같은 키 안 = 번호 순) →
		// 깊이가 모두 같지 않으면 키 묶음마다 (깊이, 번호) 비교 정렬 (항목이 많으면 묶음끼리 병렬 — 묶음마다 독립)
		bool TryGroupSort(std::vector<FInstanceSortItem>& Items)
		{
			const uint32 Count       = static_cast<uint32>(Items.size());
			const float  FirstDepth  = Items[0].Depth;
			bool         bSameDepth  = true;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				const FInstanceSortItem& Item = Items[Index];
				if ((Index > 0 && Item.Instance <= Items[Index - 1].Instance) || Item.Depth != Item.Depth)
				{
					return false;
				}
				bSameDepth &= Item.Depth == FirstDepth; // −0 == +0 (비교 정렬에서도 같은 깊이)
			}

			FSortScratch& Scratch = SortScratch;
			Scratch.Distinct.clear();
			Scratch.SlotOfItem.resize(Count);
			ResetTable(Scratch, 256);
			uint64 LastKey  = 0;
			uint32 LastSlot = EmptySlot;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				const uint64 Key = Items[Index].Key;
				if (LastSlot == EmptySlot || Key != LastKey)
				{
					LastKey  = Key;
					LastSlot = FindOrAddKey(Scratch, Key);
				}
				Scratch.SlotOfItem[Index] = LastSlot;
			}

			// 고유 키 순서 → 묶음 시작 위치
			const uint32 DistinctCount = static_cast<uint32>(Scratch.Distinct.size());
			Scratch.Order.resize(DistinctCount);
			for (uint32 Slot = 0; Slot < DistinctCount; ++Slot)
			{
				Scratch.Order[Slot] = Slot;
			}
			std::sort(Scratch.Order.begin(), Scratch.Order.end(), [&Scratch](uint32 A, uint32 B) { return Scratch.Distinct[A] < Scratch.Distinct[B]; });
			Scratch.GroupStart.assign(DistinctCount, 0);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				++Scratch.GroupStart[Scratch.SlotOfItem[Index]];
			}
			Scratch.GroupBegin.resize(DistinctCount + 1);
			uint32 Offset = 0;
			for (uint32 Rank = 0; Rank < DistinctCount; ++Rank)
			{
				const uint32 Slot        = Scratch.Order[Rank];
				const uint32 GroupCount  = Scratch.GroupStart[Slot];
				Scratch.GroupStart[Slot] = Offset;
				Scratch.GroupBegin[Rank] = Offset;
				Offset += GroupCount;
			}
			Scratch.GroupBegin[DistinctCount] = Count;

			// 안정 분배
			Scratch.Sorted.resize(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				Scratch.Sorted[Scratch.GroupStart[Scratch.SlotOfItem[Index]]++] = Items[Index];
			}

			if (!bSameDepth)
			{
				FInstanceSortItem* Sorted     = Scratch.Sorted.data();
				const uint32*      GroupBegin = Scratch.GroupBegin.data();
				const auto SortGroups = [Sorted, GroupBegin](uint32 BeginRank, uint32 EndRank) {
					for (uint32 Rank = BeginRank; Rank < EndRank; ++Rank)
					{
						std::sort(Sorted + GroupBegin[Rank], Sorted + GroupBegin[Rank + 1], [](const FInstanceSortItem& A, const FInstanceSortItem& B) {
							if (A.Depth != B.Depth)
							{
								return A.Depth < B.Depth;
							}
							return A.Instance < B.Instance;
						});
					}
				};
				constexpr uint32 ParallelMinItems = 2048; // 이보다 적으면 작업자 깨우는 비용이 더 크다
				if (Count >= ParallelMinItems && DistinctCount > 1)
				{
					FParallel::ParallelFor(DistinctCount, 1, SortGroups); // 다른 병렬 루프 안이면 그 자리에서 순차
				}
				else
				{
					SortGroups(0, DistinctCount);
				}
			}
			Items.swap(Scratch.Sorted);
			return true;
		}
	} // namespace

	void SortFrontToBack(std::vector<FInstanceSortItem>& Items)
	{
		// 적은 항목은 비교 정렬이 더 싸다. 키 묶음 정렬 조건이 안 맞으면(번호 순이 아닌 입력, NaN) 비교 정렬 — 두 경로의 결과는 같다
		constexpr size_t GroupSortMinItems = 64;
		if (Items.size() < GroupSortMinItems || !TryGroupSort(Items))
		{
			std::sort(Items.begin(), Items.end(), LessFrontToBack);
		}
	}

	void MergeSortedLists(const std::vector<FInstanceSortItem>* const* Lists, uint32 ListCount, std::vector<FInstanceSortItem>& Out)
	{
		struct FHead
		{
			const FInstanceSortItem* Current = nullptr;
			const FInstanceSortItem* End     = nullptr;
		};
		std::vector<FHead> Heads(ListCount);
		size_t             Total = 0;
		for (uint32 Index = 0; Index < ListCount; ++Index)
		{
			Heads[Index] = { Lists[Index]->data(), Lists[Index]->data() + Lists[Index]->size() };
			Total += Lists[Index]->size();
		}
		Out.clear();
		Out.reserve(Total);
		// 가장 작은 머리를 고르고, 그 목록에서 둘째로 작은 머리보다 앞서는 동안 한꺼번에 옮긴다 (같으면 앞 목록 먼저 — 안정)
		const auto Precedes = [](const FInstanceSortItem& A, uint32 ListA, const FInstanceSortItem& B, uint32 ListB) {
			return LessFrontToBack(A, B) || (!LessFrontToBack(B, A) && ListA < ListB);
		};
		for (;;)
		{
			// 한 번 훑어 가장 작은 머리(Best)와 둘째(Second)
			uint32 Best   = ListCount;
			uint32 Second = ListCount;
			for (uint32 Index = 0; Index < ListCount; ++Index)
			{
				if (Heads[Index].Current == Heads[Index].End)
				{
					continue;
				}
				if (Best == ListCount || Precedes(*Heads[Index].Current, Index, *Heads[Best].Current, Best))
				{
					Second = Best;
					Best   = Index;
				}
				else if (Second == ListCount || Precedes(*Heads[Index].Current, Index, *Heads[Second].Current, Second))
				{
					Second = Index;
				}
			}
			if (Best == ListCount)
			{
				return;
			}
			FHead& Head = Heads[Best];
			do
			{
				Out.push_back(*Head.Current);
				++Head.Current;
			} while (Head.Current != Head.End && (Second == ListCount || Precedes(*Head.Current, Best, *Heads[Second].Current, Second)));
		}
	}

	void BuildSorted(const std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches)
	{
		OutIndices.clear();
		OutBatches.clear();
		MergeSorted(Items, OutIndices, OutBatches);
	}

	void Build(std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches)
	{
		OutIndices.clear();
		OutBatches.clear();
		SortFrontToBack(Items);
		MergeSorted(Items, OutIndices, OutBatches);
	}

	void BuildBackToFront(std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches)
	{
		OutIndices.clear();
		OutBatches.clear();
		std::sort(Items.begin(), Items.end(), [](const FInstanceSortItem& A, const FInstanceSortItem& B) {
			if (A.Depth != B.Depth)
			{
				return A.Depth > B.Depth; // 먼 것부터
			}
			if (A.Key != B.Key)
			{
				return A.Key < B.Key; // 같은 깊이면 같은 키끼리 붙어 묶이게
			}
			return A.Instance < B.Instance;
		});
		MergeSorted(Items, OutIndices, OutBatches);
	}
} // namespace InstanceBatching

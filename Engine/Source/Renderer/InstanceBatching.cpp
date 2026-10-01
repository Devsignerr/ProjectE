#include "Renderer/InstanceBatching.h"

#include <algorithm>

namespace InstanceBatching
{
	void Build(std::vector<FInstanceSortItem>& Items, std::vector<uint32>& OutIndices, std::vector<FInstanceBatch>& OutBatches)
	{
		OutIndices.clear();
		OutBatches.clear();
		std::sort(Items.begin(), Items.end(), [](const FInstanceSortItem& A, const FInstanceSortItem& B) {
			if (A.Key != B.Key)
			{
				return A.Key < B.Key;
			}
			if (A.Depth != B.Depth)
			{
				return A.Depth < B.Depth;
			}
			return A.Instance < B.Instance; // 같은 깊이면 번호 순 (결과가 실행마다 같도록)
		});

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
				OutBatches.push_back({ static_cast<uint32>(OutIndices.size()), 1u, Item.Instance });
			}
			OutIndices.push_back(Item.Instance);
		}
	}
} // namespace InstanceBatching

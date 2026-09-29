#pragma once

#include "Core/Containers/Handle.h"

#include <memory>
#include <vector>

// 세대 검증 핸들로 접근하는 리소스 풀. 슬롯을 재사용하되 오래된 핸들은 무효 처리된다.
template <typename TResource, typename THandleType>
class TResourcePool
{
public:
	THandleType Add(std::unique_ptr<TResource> Resource)
	{
		uint32 Index;
		if (!FreeIndices.empty())
		{
			Index = FreeIndices.back();
			FreeIndices.pop_back();
		}
		else
		{
			Index = static_cast<uint32>(Slots.size());
			Slots.emplace_back();
		}

		Slots[Index].Resource = std::move(Resource);
		return THandleType{ Index, Slots[Index].Generation };
	}

	bool IsValid(THandleType Handle) const
	{
		return Handle.IsValid() && Handle.Index < Slots.size() && Slots[Handle.Index].Generation == Handle.Generation &&
		       Slots[Handle.Index].Resource != nullptr;
	}

	TResource* Get(THandleType Handle) const
	{
		return IsValid(Handle) ? Slots[Handle.Index].Resource.get() : nullptr;
	}

	// 슬롯을 비우고 세대를 올린다. 소유권을 호출자에게 넘긴다 (지연 해제 등).
	std::unique_ptr<TResource> Remove(THandleType Handle)
	{
		if (!IsValid(Handle))
		{
			return nullptr;
		}
		std::unique_ptr<TResource> Resource = std::move(Slots[Handle.Index].Resource);
		++Slots[Handle.Index].Generation;
		FreeIndices.push_back(Handle.Index);
		return Resource;
	}

	// 모든 살아 있는 리소스 순회: Func(THandleType, TResource&)
	template <typename TFunc>
	void ForEach(TFunc&& Func)
	{
		for (uint32 Index = 0; Index < Slots.size(); ++Index)
		{
			if (Slots[Index].Resource)
			{
				Func(THandleType{ Index, Slots[Index].Generation }, *Slots[Index].Resource);
			}
		}
	}

	size_t GetCount() const { return Slots.size() - FreeIndices.size(); }

	void Clear()
	{
		Slots.clear();
		FreeIndices.clear();
	}

private:
	struct FSlot
	{
		std::unique_ptr<TResource> Resource;
		uint32                     Generation = 0;
	};

	std::vector<FSlot>  Slots;
	std::vector<uint32> FreeIndices;
};

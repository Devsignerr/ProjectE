#pragma once

#include "Core/ECS/Entity.h"

#include <algorithm>
#include <vector>

// 에디터 다중 선택 집합. 선택 순서를 유지하며 마지막 요소가 주 선택(기즈모/인스펙터 대상)이다.
class FEntitySelection
{
public:
	// 단일 선택으로 교체 (NullEntity면 해제)
	void Set(FEntity Entity)
	{
		Entities.clear();
		Add(Entity);
	}

	// 추가 (이미 있으면 주 선택으로 올린다)
	void Add(FEntity Entity)
	{
		if (!Entity.IsValid())
		{
			return;
		}
		Remove(Entity);
		Entities.push_back(Entity);
	}

	void Remove(FEntity Entity)
	{
		Entities.erase(std::remove(Entities.begin(), Entities.end(), Entity), Entities.end());
	}

	// Ctrl+클릭: 있으면 제거, 없으면 추가
	void Toggle(FEntity Entity)
	{
		if (Contains(Entity))
		{
			Remove(Entity);
		}
		else
		{
			Add(Entity);
		}
	}

	// 목록으로 교체. Primary가 목록에 있으면 주 선택으로 둔다
	void SetMany(const std::vector<FEntity>& InEntities, FEntity Primary)
	{
		Entities.clear();
		for (FEntity Entity : InEntities)
		{
			Add(Entity);
		}
		if (Contains(Primary))
		{
			Add(Primary);
		}
	}

	void Clear() { Entities.clear(); }

	// Pred(Entity)가 true인 요소 제거 (파괴된 엔티티 정리 등)
	template <typename TPred>
	void RemoveIf(TPred&& Pred)
	{
		Entities.erase(std::remove_if(Entities.begin(), Entities.end(), Pred), Entities.end());
	}

	bool    Contains(FEntity Entity) const { return std::find(Entities.begin(), Entities.end(), Entity) != Entities.end(); }
	bool    IsEmpty() const { return Entities.empty(); }
	size_t  Num() const { return Entities.size(); }
	FEntity GetPrimary() const { return Entities.empty() ? NullEntity : Entities.back(); }

	const std::vector<FEntity>& GetEntities() const { return Entities; }

	// Shift+클릭 범위: 표시 순서(Order)에서 Anchor~Target 사이 (양끝 포함). Anchor가 없으면 Target만
	static std::vector<FEntity> GetRange(const std::vector<FEntity>& Order, FEntity Anchor, FEntity Target)
	{
		const auto AnchorIt = std::find(Order.begin(), Order.end(), Anchor);
		const auto TargetIt = std::find(Order.begin(), Order.end(), Target);
		if (TargetIt == Order.end())
		{
			return {};
		}
		if (AnchorIt == Order.end())
		{
			return { Target };
		}
		const auto First = std::min(AnchorIt, TargetIt);
		const auto Last  = std::max(AnchorIt, TargetIt);
		return std::vector<FEntity>(First, Last + 1);
	}

private:
	std::vector<FEntity> Entities;
};

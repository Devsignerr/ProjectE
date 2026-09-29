#pragma once

#include "Core/Assert.h"
#include "Core/ECS/Entity.h"

#include <utility>
#include <vector>

// 컴포넌트 풀의 타입 소거 인터페이스 (레지스트리가 엔티티 파괴 시 사용)
class FSparseSetBase
{
public:
	virtual ~FSparseSetBase() = default;

	virtual bool Contains(FEntity Entity) const = 0;
	virtual void Remove(FEntity Entity)         = 0;
	virtual size_t Size() const                 = 0;

	// 컴포넌트를 가진 엔티티 목록 (밀집 배열 순서)
	virtual const std::vector<FEntity>& GetEntities() const = 0;
};

// 희소 집합 기반 컴포넌트 풀.
// Sparse[엔티티 인덱스] → 밀집 인덱스, Dense/Components는 빈틈없이 연속 저장되어 순회가 빠르다.
// 제거는 마지막 원소와 교환(swap-remove)하므로 순회 중 제거는 허용하지 않는다.
template <typename T>
class TSparseSet final : public FSparseSetBase
{
public:
	using FComponentType = T;

	template <typename... TArgs>
	T& Emplace(FEntity Entity, TArgs&&... Args)
	{
		E_CHECKF(Entity.IsValid(), "유효하지 않은 엔티티에 컴포넌트를 추가할 수 없습니다");
		E_CHECKF(!Contains(Entity), "엔티티 {}에 이미 같은 컴포넌트가 있습니다", Entity.Index);

		if (Entity.Index >= Sparse.size())
		{
			Sparse.resize(static_cast<size_t>(Entity.Index) + 1, InvalidDense);
		}

		Sparse[Entity.Index] = static_cast<uint32>(Dense.size());
		Dense.push_back(Entity);
		return Components.emplace_back(std::forward<TArgs>(Args)...);
	}

	bool Contains(FEntity Entity) const override
	{
		if (!Entity.IsValid() || Entity.Index >= Sparse.size())
		{
			return false;
		}
		const uint32 DenseIndex = Sparse[Entity.Index];
		// 세대까지 비교해 재사용된 슬롯의 오래된 핸들을 걸러낸다
		return DenseIndex != InvalidDense && Dense[DenseIndex] == Entity;
	}

	T* TryGet(FEntity Entity)
	{
		return Contains(Entity) ? &Components[Sparse[Entity.Index]] : nullptr;
	}

	const T* TryGet(FEntity Entity) const
	{
		return Contains(Entity) ? &Components[Sparse[Entity.Index]] : nullptr;
	}

	T& Get(FEntity Entity)
	{
		T* Component = TryGet(Entity);
		E_CHECKF(Component != nullptr, "엔티티 {}에 요청한 컴포넌트가 없습니다", Entity.Index);
		return *Component;
	}

	const T& Get(FEntity Entity) const
	{
		const T* Component = TryGet(Entity);
		E_CHECKF(Component != nullptr, "엔티티 {}에 요청한 컴포넌트가 없습니다", Entity.Index);
		return *Component;
	}

	void Remove(FEntity Entity) override
	{
		if (!Contains(Entity))
		{
			return;
		}

		const uint32 DenseIndex = Sparse[Entity.Index];
		const uint32 LastIndex  = static_cast<uint32>(Dense.size() - 1);

		if (DenseIndex != LastIndex)
		{
			// 마지막 원소를 빈자리로 이동
			const FEntity LastEntity = Dense[LastIndex];
			Dense[DenseIndex]        = LastEntity;
			Components[DenseIndex]   = std::move(Components[LastIndex]);
			Sparse[LastEntity.Index] = DenseIndex;
		}

		Dense.pop_back();
		Components.pop_back();
		Sparse[Entity.Index] = InvalidDense;
	}

	size_t Size() const override { return Dense.size(); }
	bool   IsEmpty() const { return Dense.empty(); }

	const std::vector<FEntity>& GetEntities() const override { return Dense; }
	std::vector<T>&             GetComponents() { return Components; }
	const std::vector<T>&       GetComponents() const { return Components; }

	// (엔티티, 컴포넌트&) 순회. 순회 중 이 풀에 대한 추가/제거 금지.
	template <typename TFunc>
	void Each(TFunc&& Func)
	{
		for (size_t Index = 0; Index < Dense.size(); ++Index)
		{
			Func(Dense[Index], Components[Index]);
		}
	}

private:
	static constexpr uint32 InvalidDense = ~0u;

	std::vector<uint32>  Sparse;
	std::vector<FEntity> Dense;
	std::vector<T>       Components;
};

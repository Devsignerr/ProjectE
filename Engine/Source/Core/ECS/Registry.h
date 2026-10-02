#pragma once

#include "Core/ECS/SparseSet.h"

#include <memory>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <vector>

class FRegistry;

// 여러 컴포넌트를 모두 가진 엔티티를 순회하는 뷰. 가장 작은 풀을 기준으로 순회한다.
template <typename... TComponents>
class TView
{
public:
	explicit TView(FRegistry& InRegistry)
		: Registry(InRegistry)
	{
	}

	// Func(FEntity, TComponents&...) 호출. 순회 중 해당 컴포넌트 타입의 추가/제거 금지.
	template <typename TFunc>
	void Each(TFunc&& Func);

	size_t Count();

private:
	FRegistry& Registry;
};

// ECS 레지스트리: 엔티티 수명 관리 + 컴포넌트 타입별 희소 집합 풀
class FRegistry
{
public:
	FRegistry()                            = default;
	FRegistry(const FRegistry&)            = delete;
	FRegistry& operator=(const FRegistry&) = delete;

	// ---- 엔티티

	FEntity Create()
	{
		FEntity Entity;
		if (!FreeIndices.empty())
		{
			Entity.Index = FreeIndices.back();
			FreeIndices.pop_back();
		}
		else
		{
			Entity.Index = static_cast<uint32>(Generations.size());
			Generations.push_back(0);
		}
		Entity.Generation = Generations[Entity.Index];
		++AliveCount;
		return Entity;
	}

	// 모든 컴포넌트를 제거하고 슬롯을 재사용 목록에 넣는다 (세대 증가로 기존 핸들 무효화)
	void Destroy(FEntity Entity)
	{
		if (!IsValid(Entity))
		{
			return;
		}
		for (std::unique_ptr<FSparseSetBase>& Pool : Pools)
		{
			if (Pool)
			{
				Pool->Remove(Entity);
			}
		}
		++Generations[Entity.Index];
		FreeIndices.push_back(Entity.Index);
		--AliveCount;
	}

	bool IsValid(FEntity Entity) const
	{
		return Entity.IsValid() && Entity.Index < Generations.size() && Generations[Entity.Index] == Entity.Generation &&
		       !IsFree(Entity.Index);
	}

	uint32 GetAliveCount() const { return AliveCount; }

	// ---- 컴포넌트

	template <typename T, typename... TArgs>
	T& Emplace(FEntity Entity, TArgs&&... Args)
	{
		E_CHECKF(IsValid(Entity), "파괴되었거나 유효하지 않은 엔티티입니다 (인덱스 {})", Entity.Index);
		return GetPool<T>().Emplace(Entity, std::forward<TArgs>(Args)...);
	}

	template <typename T, typename... TArgs>
	T& GetOrEmplace(FEntity Entity, TArgs&&... Args)
	{
		if (T* Existing = TryGet<T>(Entity))
		{
			return *Existing;
		}
		return Emplace<T>(Entity, std::forward<TArgs>(Args)...);
	}

	template <typename T>
	T& Get(FEntity Entity)
	{
		return GetPool<T>().Get(Entity);
	}

	template <typename T>
	const T& Get(FEntity Entity) const
	{
		const TSparseSet<T>* Pool = TryGetPool<T>();
		E_CHECKF(Pool != nullptr, "등록되지 않은 컴포넌트 타입입니다");
		return Pool->Get(Entity);
	}

	template <typename T>
	T* TryGet(FEntity Entity)
	{
		TSparseSet<T>* Pool = TryGetPool<T>();
		return Pool ? Pool->TryGet(Entity) : nullptr;
	}

	template <typename T>
	const T* TryGet(FEntity Entity) const
	{
		const TSparseSet<T>* Pool = TryGetPool<T>();
		return Pool ? Pool->TryGet(Entity) : nullptr;
	}

	template <typename T>
	bool Has(FEntity Entity) const
	{
		const TSparseSet<T>* Pool = TryGetPool<T>();
		return Pool && Pool->Contains(Entity);
	}

	template <typename T>
	void Remove(FEntity Entity)
	{
		if (TSparseSet<T>* Pool = TryGetPool<T>())
		{
			Pool->Remove(Entity);
		}
	}

	// 컴포넌트 타입의 풀 (없으면 생성)
	template <typename T>
	TSparseSet<T>& GetPool()
	{
		const uint32 TypeId = GetTypeId<T>();
		if (TypeId >= Pools.size())
		{
			Pools.resize(static_cast<size_t>(TypeId) + 1);
		}
		if (!Pools[TypeId])
		{
			Pools[TypeId] = std::make_unique<TSparseSet<T>>();
		}
		return static_cast<TSparseSet<T>&>(*Pools[TypeId]);
	}

	template <typename T>
	TSparseSet<T>* TryGetPool()
	{
		const uint32 TypeId = GetTypeId<T>();
		return TypeId < Pools.size() && Pools[TypeId] ? static_cast<TSparseSet<T>*>(Pools[TypeId].get()) : nullptr;
	}

	template <typename T>
	const TSparseSet<T>* TryGetPool() const
	{
		const uint32 TypeId = GetTypeId<T>();
		return TypeId < Pools.size() && Pools[TypeId] ? static_cast<const TSparseSet<T>*>(Pools[TypeId].get()) : nullptr;
	}

	template <typename... TComponents>
	TView<TComponents...> View()
	{
		return TView<TComponents...>(*this);
	}

	// 게임 모듈 핫 리로드: 타입의 ECS 타입 ID를 버린다. 이후 처음 묻는 바이너리(새로 로드한 모듈 DLL)는 새 ID를 받는다.
	// 옛 ID의 풀은 각 레지스트리에 그대로 남아 옛 DLL 코드만 접근한다(엔티티 파괴 시 함께 정리) — 새 코드가 배치가 다른 옛 풀을
	// 자기 타입으로 캐스팅하지 않으므로 구조체 멤버가 바뀌어도 안전하다. 이미 ID를 캐시한 바이너리는 계속 옛 ID를 쓴다
	static void RetireComponentTypeId(std::type_index Type);
	// 타입 ID 조회(없으면 부여) — 진단/테스트용. 템플릿 GetTypeId<T>와 같은 표를 쓴다
	static uint32 FindOrAssignComponentTypeId(std::type_index Type) { return AssignComponentTypeId(Type); }

private:
	// 컴포넌트 타입마다 프로세스 전역 고유 ID (최초 사용 순서대로 부여).
	// 엔진 DLL과 게임 모듈 DLL이 같은 타입에 같은 ID를 쓰도록 ID 표는 Core(엔진 DLL) 한 곳에 둔다.
	// 바이너리별 지역 static은 조회 결과 캐시일 뿐이다. 키는 std::type_index (MSVC는 장식 이름으로 비교 → DLL 간 동일, 익명 네임스페이스 구분)
	template <typename T>
	static uint32 GetTypeId()
	{
		static const uint32 TypeId = AssignComponentTypeId(std::type_index(typeid(T)));
		return TypeId;
	}

	static uint32 AssignComponentTypeId(std::type_index Type);

	bool IsFree(uint32 Index) const
	{
		for (uint32 FreeIndex : FreeIndices)
		{
			if (FreeIndex == Index)
			{
				return true;
			}
		}
		return false;
	}

	std::vector<uint32>                          Generations;
	std::vector<uint32>                          FreeIndices;
	std::vector<std::unique_ptr<FSparseSetBase>> Pools;
	uint32                                       AliveCount = 0;
};

// ---- TView 구현

template <typename... TComponents>
template <typename TFunc>
void TView<TComponents...>::Each(TFunc&& Func)
{
	// 모든 풀이 존재해야 결과가 있다
	std::tuple<TSparseSet<TComponents>*...> PoolTuple{ Registry.TryGetPool<TComponents>()... };
	bool bAllPoolsExist = true;
	std::apply([&](auto*... Pools) { ((bAllPoolsExist = bAllPoolsExist && Pools != nullptr), ...); }, PoolTuple);
	if (!bAllPoolsExist)
	{
		return;
	}

	// 가장 작은 풀의 엔티티 목록을 기준으로 순회
	const std::vector<FEntity>* SmallestEntities = nullptr;
	size_t                      SmallestSize     = ~static_cast<size_t>(0);
	std::apply(
		[&](auto*... Pools) {
			((Pools->Size() < SmallestSize ? (SmallestSize = Pools->Size(), SmallestEntities = &Pools->GetEntities(), 0) : 0), ...);
		},
		PoolTuple);

	// 순회 중 풀 변경을 허용하지 않으므로 목록 복사 없이 순회
	for (size_t Index = 0; Index < SmallestEntities->size(); ++Index)
	{
		const FEntity Entity = (*SmallestEntities)[Index];

		bool bHasAll = true;
		std::apply([&](auto*... Pools) { ((bHasAll = bHasAll && Pools->Contains(Entity)), ...); }, PoolTuple);
		if (!bHasAll)
		{
			continue;
		}

		std::apply([&](auto*... Pools) { Func(Entity, Pools->Get(Entity)...); }, PoolTuple);
	}
}

template <typename... TComponents>
size_t TView<TComponents...>::Count()
{
	size_t Result = 0;
	Each([&](FEntity, TComponents&...) { ++Result; });
	return Result;
}

#include "Core/ECS/Registry.h"

#include <mutex>
#include <unordered_map>

namespace
{
	// 프로세스 전역 (엔진 DLL 안에 하나). 첫 사용 순서대로 0, 1, 2 ... (버린 ID는 다시 쓰지 않는다)
	struct FComponentTypeIdTable
	{
		std::mutex                                  Mutex;
		std::unordered_map<std::type_index, uint32> Ids;
		uint32                                      NextId = 0;
	};

	FComponentTypeIdTable& GetComponentTypeIdTable()
	{
		static FComponentTypeIdTable Table;
		return Table;
	}
} // namespace

uint32 FRegistry::AssignComponentTypeId(std::type_index Type)
{
	FComponentTypeIdTable& Table = GetComponentTypeIdTable();
	std::scoped_lock       Lock(Table.Mutex);
	const auto [It, bInserted] = Table.Ids.try_emplace(Type, Table.NextId);
	if (bInserted)
	{
		++Table.NextId;
	}
	return It->second;
}

void FRegistry::RetireComponentTypeId(std::type_index Type)
{
	FComponentTypeIdTable& Table = GetComponentTypeIdTable();
	std::scoped_lock       Lock(Table.Mutex);
	Table.Ids.erase(Type);
}

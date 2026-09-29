#include "Core/ECS/Registry.h"

#include <mutex>
#include <unordered_map>

uint32 FRegistry::AssignComponentTypeId(std::type_index Type)
{
	// 프로세스 전역 (엔진 DLL 안에 하나). 첫 사용 순서대로 0, 1, 2 ...
	static std::mutex                                  Mutex;
	static std::unordered_map<std::type_index, uint32> Ids;

	std::scoped_lock Lock(Mutex);
	return Ids.try_emplace(Type, static_cast<uint32>(Ids.size())).first->second;
}

#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <unordered_map>
#include <vector>

class FPhysicsWorld;
class FScene;

// 폴리지 충돌 (나무 등 bCollision 타입): 컴포넌트마다 정적 바디 하나 = 캡슐들의 StaticCompoundShape (Jolt 최대 바디 수를 아끼려고).
// 캡슐은 인스턴스 위치에서 위로 서 있다 (크기 배율 적용, 지면 정렬 기울기는 무시). 에셋 ChangeCounter가 바뀌면 다시 만든다.
// FPhysicsSystem::Update가 지형 충돌 다음에 부른다. UserData = 폴리지 엔티티 ToId
class FFoliageCollision
{
public:
	void   Sync(FScene& Scene, FPhysicsWorld& World);
	uint32 GetBodyCount() const { return static_cast<uint32>(Bodies.size()); }

private:
	struct FBody
	{
		uint32      Body          = ~0u;
		const void* Asset         = nullptr;
		uint64      ChangeCounter = 0;
		uint64      LastSeen      = 0;
	};
	std::unordered_map<FEntity, FBody> Bodies;
	uint64                             Frame = 0;
};

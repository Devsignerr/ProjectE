#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <unordered_map>
#include <vector>

class FPhysicsWorld;
class FScene;
struct FTerrainData;

// 지형 높이맵 충돌 (Jolt HeightFieldShape, 정적 바디). FPhysicsSystem::Update가 바디 동기화 뒤에 부른다.
//   - 대상: bCollision인 FTerrainComponent (데이터는 FTerrainLibrary 공유)
//   - Jolt 높이장은 Y-up: 바디 회전 = 축 순환(Jolt X→엔진 Y, Y→Z, Z→X, 쿼터니언 (0.5, 0.5, 0.5, 0.5)),
//     Jolt 표본 (x, y) = 엔진 격자 (GridX = y, GridY = x) — 전치라 셀 대각선 (0,0)-(1,1)이 렌더/TerrainMath와 같다
//   - 데이터 ChangeCounter·위치·크기가 바뀌면 바디를 다시 만든다 (브러시 편집 반영)
//   - UserData = 엔티티 ToId → 레이캐스트/캐릭터 접촉이 지형 엔티티를 돌려준다
class FTerrainCollision
{
public:
	void Sync(FScene& Scene, FPhysicsWorld& World);
	void Clear(FPhysicsWorld* World); // World가 이미 사라졌으면 nullptr (핸들만 버린다)
	uint32 GetBodyCount() const { return static_cast<uint32>(Bodies.size()); }

	// 높이장 표본 (Jolt 순서, 미터): 테스트/디버그용 순수 변환
	static void BuildSamples(const FTerrainData& Data, float HeightScaleMeters, std::vector<float>& OutSamples);

private:
	struct FBody
	{
		uint32       Body = ~0u;
		const void*  Data = nullptr;
		uint64       ChangeCounter = 0;
		float        Key[6]        = {}; // 위치 XYZ, 크기 XY, 높이 범위
		uint64       LastSeen      = 0;
	};
	std::unordered_map<FEntity, FBody> Bodies;
	uint64                             Frame = 0;
};

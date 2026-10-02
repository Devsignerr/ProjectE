#include "Core/ECS/Registry.h"
#include "Core/Testing/TestFramework.h"

namespace
{
	struct FRetireProbeA
	{
		int32 Value = 0;
	};
	struct FRetireProbeB
	{
		float Value = 0.0f;
	};
	struct FRetireProbeC
	{
		bool bValue = false;
	};
} // namespace

// 게임 모듈 핫 리로드: 폐기한 ECS 타입 ID는 다시 쓰지 않고, 같은 타입을 다시 물으면 새 ID를 받는다
E_TEST(ECS_RetireComponentTypeId)
{
	const uint32 IdA = FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeA));
	const uint32 IdB = FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeB));
	E_EXPECT_TRUE(IdA != IdB);
	E_EXPECT_EQ(FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeA)), IdA); // 같은 타입 = 같은 ID

	FRegistry::RetireComponentTypeId(typeid(FRetireProbeA));
	const uint32 NewIdA = FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeA));
	E_EXPECT_TRUE(NewIdA != IdA && NewIdA != IdB);
	E_EXPECT_EQ(FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeA)), NewIdA);
	E_EXPECT_EQ(FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeB)), IdB); // 다른 타입은 그대로

	// 폐기 뒤 새 타입도 기존 ID와 겹치지 않는다 (표 크기로 번호를 매기면 겹침)
	const uint32 IdC = FRegistry::FindOrAssignComponentTypeId(typeid(FRetireProbeC));
	E_EXPECT_TRUE(IdC != IdA && IdC != IdB && IdC != NewIdA);
}

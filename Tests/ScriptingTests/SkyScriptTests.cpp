// Lua Sky 테이블 (Phase 49, ScriptSkyBindings.cpp) + 시간대 시스템 (FTimeOfDaySystem — FGameWorld::TickPresentation)
#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SkyAtmosphere.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <cmath>
#include <filesystem>

E_TEST(SkyScript_SunAnglesAndTimeOfDay)
{
	FScene        Scene;
	const FEntity Sun = Scene.CreateEntity("Sun");
	Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun);
	const FEntity Sky = Scene.CreateEntity("Sky");
	Scene.GetRegistry().Emplace<FTimeOfDayComponent>(Sky).TimeOfDay = 9.0f;

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, FTestRegistry::GetTempDirectory() });
	World.BeginPlay(Scene);

	// 태양 각도: 설정한 값이 그대로 읽힌다 (방향광 전방 = -태양 방향)
	E_EXPECT_TRUE(Scripts.RunString("assert(Sky.SetSunAngles(30, 90))"));
	E_EXPECT_TRUE(Scripts.RunString("local e, a = Sky.GetSunAngles(); assert(math.abs(e - 30) < 0.01 and math.abs(a - 90) < 0.01)"));
	const FVector3 Forward = Scene.GetRegistry().Get<FTransformComponent>(Sun).Rotation.GetForwardVector();
	E_EXPECT_NEAR(FVector3::Dot(Forward, -FSunMath::SunDirectionFromAngles(30.0f, 90.0f)), 1.0f, 1.0e-4f);

	// 시간대: 24시간으로 감기고, 표시 틱이 태양을 그 시각으로 돌린다
	E_EXPECT_TRUE(Scripts.RunString("assert(Sky.SetTimeOfDay(26)); assert(math.abs(Sky.GetTimeOfDay() - 2) < 0.001)"));
	World.TickPresentation(Scene, 0.0f);
	float Elevation = 0.0f;
	float Azimuth   = 0.0f;
	E_EXPECT_TRUE(FSkyScene::GetSunAngles(Scene, Elevation, Azimuth));
	E_EXPECT_TRUE(Elevation < 0.0f); // 새벽 2시 = 지평선 아래

	// 플레이 중에는 하루 길이대로 흐른다 (1분에 하루 → 1초에 0.4시간)
	Scene.GetRegistry().Get<FTimeOfDayComponent>(Sky).DayLengthMinutes = 1.0f;
	World.TickPresentation(Scene, 1.0f);
	E_EXPECT_NEAR(Scene.GetRegistry().Get<FTimeOfDayComponent>(Sky).TimeOfDay, 2.4f, 1.0e-3f);

	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	World.EndPlay();
}

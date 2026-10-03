// Lua 하늘/시간대 (Phase 49) — Scene/SkyAtmosphere.h FSkyScene (씬의 첫 방향광 = 태양, 첫 시간대 컴포넌트)
//   Sky.SetSunAngles(elevation, azimuth) -> bool   -- 고도각(수평 위 +)·방위각(+X에서 +Y 쪽, 도)으로 첫 방향광을 돌린다
//   local elevation, azimuth = Sky.GetSunAngles()   -- 방향광이 없으면 nil
//   Sky.SetTimeOfDay(hours) -> bool                 -- 시간대 컴포넌트 시각 (0~24로 감김, 컴포넌트가 없으면 false)
//   Sky.GetTimeOfDay() -> hours | nil
// 시간대 컴포넌트가 있으면 매 프레임 그 시각으로 태양을 다시 맞추므로 SetSunAngles보다 SetTimeOfDay를 쓴다
#include "Scripting/LuaRuntime.h"

#include "Scene/SkyAtmosphere.h"

void FLuaRuntime::RegisterSkyBindings()
{
	sol::table SkyTable = Lua.create_named_table("Sky");
	SkyTable["SetSunAngles"] = [this](float Elevation, float Azimuth) {
		return Scene != nullptr && FSkyScene::SetSunAngles(*Scene, Elevation, Azimuth);
	};
	SkyTable["GetSunAngles"] = [this](sol::this_state State) -> sol::variadic_results {
		sol::variadic_results Results;
		float Elevation = 0.0f;
		float Azimuth   = 0.0f;
		if (Scene != nullptr && FSkyScene::GetSunAngles(*Scene, Elevation, Azimuth))
		{
			Results.push_back(sol::make_object(State, Elevation));
			Results.push_back(sol::make_object(State, Azimuth));
		}
		else
		{
			Results.push_back(sol::make_object(State, sol::lua_nil));
		}
		return Results;
	};
	SkyTable["SetTimeOfDay"] = [this](float Hours) { return Scene != nullptr && FSkyScene::SetTimeOfDay(*Scene, Hours); };
	SkyTable["GetTimeOfDay"] = [this]() -> sol::optional<float> {
		float Hours = 0.0f;
		if (Scene != nullptr && FSkyScene::GetTimeOfDay(*Scene, Hours))
		{
			return Hours;
		}
		return sol::nullopt;
	};
}

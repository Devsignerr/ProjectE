// Lua Debug.Draw* → FDebugDraw (FScriptDebugDrawHooks, FGameWorld가 연결 — Phase 41-3)
#include "Core/Testing/TestFramework.h"
#include "Renderer/DebugDraw.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	std::filesystem::path WriteDebugDrawScript()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEDebugDrawScriptTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/DebugDrawer.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local D = {}
function D:OnStart()
	Debug.DrawLine(Vector3(0, 0, 0), Vector3(0, 0, 100), Vector3(1, 0, 0), 1.0) -- 1초 (OnStart에서 한 번)
end
function D:OnUpdate(dt)
	Debug.DrawLine(Vector3(0, 0, 0), Vector3(100, 0, 0))                          -- 1
	Debug.DrawArrow(Vector3(0, 0, 0), Vector3(0, 100, 0), Vector4(0, 1, 0, 0.5))  -- 5
	Debug.DrawBox(Vector3(0, 0, 50), Vector3(10, 10, 10), nil, 0, Quat.FromEuler(0, 45, 0), true) -- 12 (항상 위)
	Debug.DrawSphere(Vector3(0, 0, 0), 30, Vector3(0, 0, 1))                     -- 3 × 16
	Debug.DrawCapsule(Vector3(0, 0, 0), 10, 40, Vector3(1, 1, 0))                 -- 2 × 16 + 4 + 4 × 8
end
return D
)";
		return Directory;
	}

	size_t CountOnTop(const FDebugDraw& Draw)
	{
		size_t Count = 0;
		for (const FDebugLine& Line : Draw.GetLines())
		{
			Count += Line.bDepthTest ? 0 : 1;
		}
		return Count;
	}
} // namespace

E_TEST(DebugDrawScript_LuaDrawsIntoStoreAndClearsOnEndPlay)
{
	const std::filesystem::path Content = WriteDebugDrawScript();
	FScene                      Scene;
	const FEntity               Drawer = Scene.CreateEntity("Drawer");
	Scene.GetRegistry().Emplace<FScriptComponent>(Drawer).ScriptAsset = "Scripts/DebugDrawer.lua";

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	FDebugDraw& Draw = FDebugDraw::Get();
	E_EXPECT_FALSE(Draw.IsEnabled()); // Resources 없음 = GPU 없는 서버 취급 → 무시
	World.TickGameplay(0.1f, nullptr);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(0));

	Draw.SetEnabled(true); // 그리는 경로 검증 (GPU 앱과 같은 상태)
	constexpr size_t PerFrame = 1 + 5 + 12 + 48 + (32 + 4 + 32);
	World.TickGameplay(0.1f, nullptr);
	E_EXPECT_EQ(Draw.GetLines().size(), PerFrame);
	E_EXPECT_EQ(CountOnTop(Draw), static_cast<size_t>(12));
	// 다음 틱: 지난 틱의 한 프레임 선은 Tick에서 사라지고 다시 그려진다 (쌓이지 않는다)
	World.TickGameplay(0.1f, nullptr);
	E_EXPECT_EQ(Draw.GetLines().size(), PerFrame);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);

	// 잘못된 색은 스크립트 오류
	E_EXPECT_TRUE(!Scripts.RunString("Debug.DrawLine(Vector3(0, 0, 0), Vector3(1, 0, 0), 'red')"));

	World.EndPlay();
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(0)); // 편집 화면에 남지 않는다
	Draw.SetEnabled(true);
}

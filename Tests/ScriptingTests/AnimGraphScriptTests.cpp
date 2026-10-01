#include "Core/Testing/TestFramework.h"
#include "Scene/AnimGraph.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

// Lua 애니메이션 그래프 파라미터: 캐릭터 루트(조상)에서 설정해도 자손의 AnimGraphComponent로 간다
E_TEST(AnimGraphScript_SetAndGetParams)
{
	FScene        Scene;
	const FEntity Character = Scene.CreateEntity("Character");
	const FEntity Model     = Scene.CreateEntity("Model");
	Scene.SetParent(Model, Character);
	Scene.GetRegistry().Emplace<FAnimGraphComponent>(Model);
	Scene.CreateEntity("NoGraph");

	FScriptSystem Scripts;
	Scripts.BeginPlay(Scene);
	E_EXPECT_TRUE(Scripts.RunString(R"(
		local C = Scene.Find('Character')
		assert(C:SetAnimParam('Speed', 120.5))
		assert(C:SetAnimParam('Crouch', true))
		assert(C:GetAnimParam('Speed') == 120.5)
		assert(C:GetAnimParam('Crouch') == 1) -- 그래프 선언이 없으면 숫자
		assert(C:GetAnimParam('Nope') == nil)
		assert(C:GetAnimState() == nil)       -- 그래프 에셋 없음
		assert(Scene.Find('NoGraph'):SetAnimParam('Speed', 1) == false)
	)"));
	float Value = 0.0f;
	E_EXPECT_TRUE(Scene.GetRegistry().Get<FAnimGraphComponent>(Model).Runtime.Parameters.TryGet("Speed", Value));
	E_EXPECT_NEAR(Value, 120.5f, 1.0e-4f);
	E_EXPECT_FALSE(Scripts.RunString("Scene.Find('Character'):SetAnimParam('Speed', 'fast')"));
	Scripts.EndPlay();
}

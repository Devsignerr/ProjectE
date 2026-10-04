#include "Core/Testing/TestFramework.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

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

// Lua 몽타주 (Phase 42-3): 캐릭터 루트 스크립트에서 PlayMontage(옵션 표) → 표시 틱이 재생 → 끝나면 OnMontageEnded(clip, interrupted, slot)
E_TEST(AnimGraphScript_MontagePlayStopAndEndEvent)
{
	const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectEMontageScript";
	std::filesystem::create_directories(Content / L"Scripts");
	{
		std::ofstream File(Content / L"Scripts/Actor.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Ended = 0, LastClip = "", LastInterrupted = false, LastSlot = "", PlayingAtStart = false, BadClip = true } }
function T:OnStart()
	self.Properties.BadClip = self.entity:PlayMontage('Nope')
	assert(self.entity:PlayMontage('Wave', { Slot = 'Upper', BlendIn = 0, BlendOut = 0.05, Speed = 2 }))
	self.Properties.PlayingAtStart = self.entity:IsMontagePlaying('Upper') and not self.entity:IsMontagePlaying('Other')
end
function T:OnMontageEnded(clip, interrupted, slot)
	self.Properties.Ended = self.Properties.Ended + 1
	self.Properties.LastClip = clip
	self.Properties.LastInterrupted = interrupted
	self.Properties.LastSlot = slot
	if self.Properties.Ended == 1 then
		assert(self.entity:PlayMontage('Wave', { Loop = true }))
		assert(self.entity:StopMontage())
	end
end
return T
)";
	}
	FAnimationClip Wave;
	Wave.Name     = "Wave";
	Wave.Duration = 0.5f;

	FScene        Scene;
	const FEntity Character = Scene.CreateEntity("Character");
	const FEntity Model     = Scene.CreateEntity("Model");
	Scene.SetParent(Model, Character);
	FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Model);
	Animation.Runtime.Set          = MakeAnimationSet({ Wave }, { -1 }, std::vector<FNodePose>(1));
	FScriptComponent& Script       = Scene.GetRegistry().Emplace<FScriptComponent>(Character);
	Script.ScriptAsset             = "Scripts/Actor.lua";

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
		World.TickPresentation(Scene, 1.0f / 60.0f);
	}
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_FALSE(Scripts.GetInstanceProperty(Character, "BadClip").bBool);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Character, "PlayingAtStart").bBool);
	// 0.5초 클립 × 2배속 = 0.25초 뒤 완료 → 이벤트 1. 그 안에서 반복 몽타주 재생 + Stop → 중단 이벤트 2 (기본 슬롯)
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Character, "Ended").Number, 2.0, 1.0e-9);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Character, "LastClip").String == "Wave");
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Character, "LastInterrupted").bBool);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Character, "LastSlot").String == "Default");
	E_EXPECT_FALSE(FAnimationSystem::IsMontagePlaying(Scene, Character, ""));
	World.EndPlay();
	std::error_code ErrorCode;
	std::filesystem::remove_all(Content, ErrorCode);
}

// 몽타주 끝 콜백도 다른 배달 경로처럼 인스턴스 범위(FInstanceScope) 안에서 불린다 — Timer.After/Coroutine.Start를 부를 수 있고,
// 콜백 오류는 그 인스턴스만 멈춘다 (다른 캐릭터의 콜백·타이머는 계속)
E_TEST(AnimGraphScript_MontageEndCallbackOwnsTimersAndCoroutines)
{
	const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectEMontageScope";
	std::filesystem::create_directories(Content / L"Scripts");
	{
		std::ofstream File(Content / L"Scripts/Good.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Ended = 0, TimerFired = false, CoroutineDone = false } }
function T:OnStart()
	assert(self.entity:PlayMontage('Wave', { BlendIn = 0, BlendOut = 0 }))
end
function T:OnMontageEnded(clip, interrupted, slot)
	self.Properties.Ended = self.Properties.Ended + 1
	Timer.After(0.05, function() self.Properties.TimerFired = true end)
	Coroutine.Start(function()
		Wait(0)
		WaitFrames(2)
		self.Properties.CoroutineDone = true
	end)
end
return T
)";
	}
	{
		std::ofstream File(Content / L"Scripts/Bad.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Ended = 0 } }
function T:OnStart()
	assert(self.entity:PlayMontage('Wave', { BlendIn = 0, BlendOut = 0 }))
end
function T:OnMontageEnded()
	self.Properties.Ended = self.Properties.Ended + 1
	Timer.After(0.01, function() self.Properties.Ended = 100 end) -- 아래 오류로 인스턴스가 멈추면 이 타이머도 지워진다
	error('몽타주 콜백 오류')
end
return T
)";
	}
	FAnimationClip Wave;
	Wave.Name     = "Wave";
	Wave.Duration = 0.2f;

	FScene     Scene;
	const auto AddCharacter = [&](const char* Name, const char* ScriptPath) {
		const FEntity        Character = Scene.CreateEntity(Name);
		FAnimationComponent& Animation = Scene.GetRegistry().Emplace<FAnimationComponent>(Character);
		Animation.Runtime.Set          = MakeAnimationSet({ Wave }, { -1 }, std::vector<FNodePose>(1));
		Scene.GetRegistry().Emplace<FScriptComponent>(Character).ScriptAsset = ScriptPath;
		return Character;
	};
	const FEntity Good = AddCharacter("Good", "Scripts/Good.lua");
	const FEntity Bad  = AddCharacter("Bad", "Scripts/Bad.lua");

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
		World.TickPresentation(Scene, 1.0f / 60.0f);
	}
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Good, "Ended").Number, 1.0, 1.0e-9);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Good, "TimerFired").bBool);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Good, "CoroutineDone").bBool);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Bad, "Ended").Number, 1.0, 1.0e-9); // 멈춘 인스턴스의 타이머는 지워졌다
	E_EXPECT_EQ(Scripts.GetErrorCount(), 1u);
	World.EndPlay();
	std::error_code ErrorCode;
	std::filesystem::remove_all(Content, ErrorCode);
}

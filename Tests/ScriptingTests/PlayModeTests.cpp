#include "Core/InputMode.h"
#include "Core/Testing/TestFramework.h"
#include "Editor/EditorContext.h"
#include "Editor/PlayMode.h"
#include "Scene/SceneSerializer.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

namespace
{
	std::filesystem::path MakePlayModeContent()
	{
		const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectEPlayModeTests";
		std::filesystem::create_directories(Directory / L"Scripts");
		std::ofstream File(Directory / L"Scripts/Push.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local Push = {}
function Push:OnUpdate(dt)
	self.entity:SetPosition(self.entity:GetPosition() + Vector3(10, 0, 0))
	if self.entity:GetName() == "Doomed" then self.entity:Destroy() end
end
return Push
)";
		return Directory;
	}
} // namespace

// 재생 → 스크립트 실행(이동/파괴/생성) → 정지 시 편집 씬이 그대로 복원되는가
E_TEST(PlayMode_StopRestoresEditScene)
{
	FScene        EditScene;
	const FEntity Mover = EditScene.CreateEntity("Mover");
	EditScene.GetRegistry().Emplace<FScriptComponent>(Mover).ScriptAsset = "Scripts/Push.lua";
	const FEntity Doomed = EditScene.CreateEntity("Doomed");
	EditScene.GetRegistry().Emplace<FScriptComponent>(Doomed).ScriptAsset = "Scripts/Push.lua";
	EditScene.UpdateTransforms();
	const std::string Before = FSceneSerializer::ToJsonString(EditScene);

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, MakePlayModeContent() }); // 물리/게임 모듈/GPU 없이
	FPlayMode PlayMode;
	PlayMode.Init(EditScene, World);

	FEditorContext Context;
	Context.Scene = &EditScene;
	Context.Select(Mover);

	PlayMode.Play(Context);
	E_EXPECT_TRUE(Context.Scene == &PlayMode.GetPlayScene());
	E_EXPECT_TRUE(Context.bPlaying);
	const FEntity PlayMover = Context.SelectedEntity; // 선택이 플레이 씬 엔티티로 옮겨졌는가
	E_EXPECT_TRUE(PlayMode.GetPlayScene().GetRegistry().IsValid(PlayMover));

	E_EXPECT_TRUE(PlayMode.Tick(Context, 0.016f, nullptr));
	E_EXPECT_TRUE(PlayMode.Tick(Context, 0.016f, nullptr));
	PlayMode.GetPlayScene().CreateEntity("SpawnedDuringPlay");
	E_EXPECT_NEAR(PlayMode.GetPlayScene().GetTransform(PlayMover).Position.X, 20.0f, 1.0e-4f);

	// 일시정지: 진행하지 않음, Step은 한 프레임만
	PlayMode.TogglePause();
	E_EXPECT_TRUE(Context.bPlaying);
	E_EXPECT_FALSE(PlayMode.Tick(Context, 0.016f, nullptr));
	E_EXPECT_TRUE(Context.bPaused);
	PlayMode.RequestStep();
	E_EXPECT_TRUE(PlayMode.Tick(Context, 0.016f, nullptr));
	E_EXPECT_FALSE(PlayMode.Tick(Context, 0.016f, nullptr));
	E_EXPECT_NEAR(PlayMode.GetPlayScene().GetTransform(PlayMover).Position.X, 30.0f, 1.0e-4f);

	PlayMode.Stop(Context);
	E_EXPECT_TRUE(Context.Scene == &EditScene);
	E_EXPECT_FALSE(Context.bPlaying);
	E_EXPECT_TRUE(Context.SelectedEntity == Mover);
	E_EXPECT_TRUE(FSceneSerializer::ToJsonString(EditScene) == Before);
	E_EXPECT_EQ(PlayMode.GetPlayScene().GetRegistry().GetAliveCount(), 0u);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_FALSE(Scripts.IsPlaying());

	// 다시 재생해도 편집 씬 기준으로 새로 시작한다
	PlayMode.Play(Context);
	E_EXPECT_EQ(PlayMode.GetPlayScene().GetRegistry().GetAliveCount(), EditScene.GetRegistry().GetAliveCount());
	PlayMode.Stop(Context);
}

// 빙의/해제 (F8): 플레이는 빙의로 시작(뷰포트 편집 불가), 해제해도 게임은 진행, 정지하면 해제.
// 입력 모드: Lua Game.SetInputMode가 전역 모드를 바꾸고, 정지(EndPlay)하면 GameAndUI로 돌아간다
E_TEST(PlayMode_PossessEjectAndInputMode)
{
	const std::filesystem::path Content = MakePlayModeContent();
	{
		std::ofstream File(Content / L"Scripts/InputModeUser.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local User = {}
function User:OnStart()
	Game.SetInputMode("UIOnly")
	assert(Game.GetInputMode() == "UIOnly")
end
function User:OnUpdate(dt)
	self.entity:SetPosition(self.entity:GetPosition() + Vector3(1, 0, 0))
end
return User
)";
	}
	FScene        EditScene;
	const FEntity User = EditScene.CreateEntity("User");
	EditScene.GetRegistry().Emplace<FScriptComponent>(User).ScriptAsset = "Scripts/InputModeUser.lua";
	EditScene.UpdateTransforms();

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	FPlayMode PlayMode;
	PlayMode.Init(EditScene, World);
	FEditorContext Context;
	Context.Scene = &EditScene;
	Context.Select(User);
	E_EXPECT_TRUE(Context.CanEditInViewport());

	FInputModeState::Set(EInputMode::GameOnly); // 이전 플레이의 값이 남아 있어도 BeginPlay가 기본값으로
	PlayMode.Play(Context);
	E_EXPECT_TRUE(FInputModeState::Get() == EInputMode::GameAndUI);
	E_EXPECT_TRUE(PlayMode.IsPossessed() && Context.bPossessed && !Context.CanEditInViewport());
	const FEntity PlayUser = Context.SelectedEntity;

	E_EXPECT_TRUE(PlayMode.Tick(Context, 0.016f, nullptr));
	E_EXPECT_TRUE(FInputModeState::Get() == EInputMode::UIOnly);

	PlayMode.SetPossessed(Context, false);
	E_EXPECT_TRUE(!PlayMode.IsPossessed() && !Context.bPossessed && Context.CanEditInViewport());
	E_EXPECT_TRUE(PlayMode.Tick(Context, 0.016f, nullptr)); // 해제 중에도 게임은 진행
	E_EXPECT_NEAR(PlayMode.GetPlayScene().GetTransform(PlayUser).Position.X, 2.0f, 1.0e-4f);
	PlayMode.SetPossessed(Context, true);
	E_EXPECT_TRUE(!Context.CanEditInViewport());

	PlayMode.Stop(Context);
	E_EXPECT_TRUE(!PlayMode.IsPossessed() && !Context.bPossessed && Context.CanEditInViewport());
	E_EXPECT_TRUE(FInputModeState::Get() == EInputMode::GameAndUI);
	PlayMode.SetPossessed(Context, true); // 플레이 중이 아니면 무시
	E_EXPECT_FALSE(PlayMode.IsPossessed());
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
}

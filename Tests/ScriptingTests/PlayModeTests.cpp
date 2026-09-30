#include "Core/Testing/TestFramework.h"
#include "Editor/EditorContext.h"
#include "Editor/PlayMode.h"
#include "Scene/SceneSerializer.h"
#include "Scripting/ScriptSystem.h"

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
	Scripts.SetContentDirectory(MakePlayModeContent());
	FPlayMode PlayMode;
	PlayMode.Init(EditScene, Scripts);

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

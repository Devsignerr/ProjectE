#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"
#include "Online/SteamSubsystem.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <filesystem>
#include <string>

E_TEST(GameScript_AppHooks)
{
	FScene        Scene;
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(FTestRegistry::GetTempDirectory());

	// 훅 없음 (서버 등): 무시하고 기본값
	Scripts.BeginPlay(Scene);
	E_EXPECT_TRUE(Scripts.RunString(R"(
Game.Quit()
Game.SetWindowMode('BorderlessFullscreen')
assert(Game.GetWindowMode() == 'Windowed' and Game.IsVSync())
assert(Game.GetVersion() == '0.1.0')
assert(not Steam.IsAvailable() and Steam.GetPlayerName() == '' and not Steam.UnlockAchievement('ACH_TEST'))
assert(not Steam.ActivateOverlay('Achievements') and not Steam.IsOverlayActive())
)"));
	// 이름 = .eproject DisplayName (TestMain이 예제 프로젝트를 연다)
	E_EXPECT_TRUE(FPaths::HasProject());
	E_EXPECT_TRUE(Scripts.RunString("assert(Game.GetName() == '" + FPaths::GetProjectDescriptor().GetDisplayName() + "')"));
	Scripts.EndPlay();

	int32       QuitCount  = 0;
	std::string WindowMode = "Windowed";
	bool        bVSync     = true;
	Scripts.SetAppHooks({
		[&]() { ++QuitCount; },
		[&]() { return WindowMode; },
		[&](const std::string& Mode) {
			if (Mode != "Windowed" && Mode != "BorderlessFullscreen")
			{
				return false;
			}
			WindowMode = Mode;
			return true;
		},
		[&]() { return bVSync; },
		[&](bool bEnabled) { bVSync = bEnabled; },
	});
	Scripts.BeginPlay(Scene);
	E_EXPECT_TRUE(Scripts.RunString(R"(
Game.SetWindowMode('BorderlessFullscreen')
assert(Game.GetWindowMode() == 'BorderlessFullscreen')
Game.SetVSync(false)
assert(not Game.IsVSync())
Game.Quit()
)"));
	E_EXPECT_EQ(QuitCount, 1);
	E_EXPECT_TRUE(WindowMode == "BorderlessFullscreen");
	E_EXPECT_FALSE(bVSync);
	// 알 수 없는 모드는 스크립트 오류
	E_EXPECT_FALSE(Scripts.RunString("Game.SetWindowMode('Exclusive')"));
	E_EXPECT_TRUE(WindowMode == "BorderlessFullscreen");
	Scripts.EndPlay();
}

E_TEST(Steam_DisabledWithoutInit)
{
	// 테스트는 Steam을 초기화하지 않는다: 모든 API가 "사용 불가"로 조용히 실패
	FSteamSubsystem& Steam = FSteamSubsystem::Get();
	E_EXPECT_TRUE(Steam.Init(0, false) == FSteamSubsystem::EInitResult::Disabled);
	E_EXPECT_FALSE(Steam.IsAvailable());
	E_EXPECT_EQ(Steam.GetAppId(), 0u);
	E_EXPECT_TRUE(Steam.GetPlayerName().empty());
	E_EXPECT_EQ(Steam.GetSteamId(), static_cast<uint64>(0));
	E_EXPECT_FALSE(Steam.UnlockAchievement("ACH_TEST"));
	E_EXPECT_FALSE(Steam.IsAchievementUnlocked("ACH_TEST"));
	E_EXPECT_FALSE(Steam.ActivateOverlay("Friends"));
	Steam.RunCallbacks(); // 초기화 전에도 안전
	Steam.Shutdown();

	// .eproject SteamAppId 저장/로드
	const std::filesystem::path File = FTestRegistry::GetTempDirectory() / L"ProjectE_Steam" / L"Steam.eproject";
	FProjectDescriptor Saved;
	Saved.Name       = "Steam";
	Saved.SteamAppId = 480;
	E_EXPECT_TRUE(Saved.SaveToFile(File));
	FProjectDescriptor Loaded;
	E_EXPECT_TRUE(Loaded.LoadFromFile(File));
	E_EXPECT_EQ(Loaded.SteamAppId, 480u);
	std::error_code ErrorCode;
	std::filesystem::remove_all(File.parent_path(), ErrorCode);
}

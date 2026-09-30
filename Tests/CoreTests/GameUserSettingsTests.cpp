#include "Core/GameUserSettings.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>

E_TEST(GameUserSettings_JsonLayering)
{
	FGameUserSettings Settings;
	E_EXPECT_TRUE(Settings.WindowMode == EWindowMode::Windowed);
	E_EXPECT_TRUE(Settings.bVSync);

	// 프로젝트 기본값: 전체 화면으로 시작
	E_EXPECT_TRUE(Settings.ApplyJson(R"({ "WindowMode": "BorderlessFullscreen", "WindowWidth": 1920, "WindowHeight": 1080 })"));
	E_EXPECT_TRUE(Settings.WindowMode == EWindowMode::BorderlessFullscreen);
	E_EXPECT_EQ(Settings.WindowWidth, 1920u);

	// 사용자 설정: 있는 키만 덮어쓴다 (대소문자 무시), 범위를 벗어난 크기는 제한
	E_EXPECT_TRUE(Settings.ApplyJson(R"({ "WindowMode": "windowed", "VSync": false, "WindowHeight": 10 })"));
	E_EXPECT_TRUE(Settings.WindowMode == EWindowMode::Windowed);
	E_EXPECT_FALSE(Settings.bVSync);
	E_EXPECT_EQ(Settings.WindowWidth, 1920u);
	E_EXPECT_EQ(Settings.WindowHeight, 240u);

	// 알 수 없는 모드/타입이 다른 값은 무시, 깨진 JSON은 실패하고 값 유지
	E_EXPECT_TRUE(Settings.ApplyJson(R"({ "WindowMode": "Exclusive", "VSync": "yes" })"));
	E_EXPECT_TRUE(Settings.WindowMode == EWindowMode::Windowed);
	E_EXPECT_FALSE(Settings.bVSync);
	E_EXPECT_FALSE(Settings.ApplyJson("{ \"VSync\": "));
	E_EXPECT_FALSE(Settings.bVSync);
}

E_TEST(GameUserSettings_FileRoundtrip)
{
	const std::filesystem::path Path = FTestRegistry::GetTempDirectory() / L"ProjectE_Settings" / L"GameUserSettings.json";

	FGameUserSettings Saved;
	Saved.WindowMode   = EWindowMode::BorderlessFullscreen;
	Saved.WindowWidth  = 1600;
	Saved.WindowHeight = 900;
	Saved.bVSync       = false;
	E_EXPECT_TRUE(Saved.SaveToFile(Path));

	FGameUserSettings Loaded;
	E_EXPECT_TRUE(Loaded.ApplyFile(Path));
	E_EXPECT_TRUE(Loaded.WindowMode == EWindowMode::BorderlessFullscreen);
	E_EXPECT_EQ(Loaded.WindowWidth, 1600u);
	E_EXPECT_EQ(Loaded.WindowHeight, 900u);
	E_EXPECT_FALSE(Loaded.bVSync);

	// 파일이 없으면 성공 (기본값 유지)
	FGameUserSettings Missing;
	E_EXPECT_TRUE(Missing.ApplyFile(Path.parent_path() / L"없음.json"));
	E_EXPECT_TRUE(Missing.WindowMode == EWindowMode::Windowed);

	EWindowMode Mode = EWindowMode::Windowed;
	E_EXPECT_TRUE(TryParseWindowMode("BORDERLESSFULLSCREEN", Mode) && Mode == EWindowMode::BorderlessFullscreen);
	E_EXPECT_FALSE(TryParseWindowMode("Fullscreen", Mode));

	std::error_code ErrorCode;
	std::filesystem::remove_all(Path.parent_path(), ErrorCode);
}

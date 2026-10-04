#include "Core/Settings/SortingLayerSettings.h"
#include "Core/Testing/TestFramework.h"

E_TEST(SortingLayer_DefaultAndResolve)
{
	FSortingLayerSettings Settings;
	E_EXPECT_EQ(Settings.GetLayerCount(), 1u);
	E_EXPECT_EQ(Settings.GetLayerName(0), std::string("Default"));
	E_EXPECT_TRUE(Settings.AddLayer("Background"));
	E_EXPECT_TRUE(Settings.AddLayer("Characters"));
	E_EXPECT_FALSE(Settings.AddLayer("Characters")); // 중복
	E_EXPECT_FALSE(Settings.AddLayer(""));           // 빈 이름

	E_EXPECT_EQ(Settings.ResolveLayer("Characters"), 2u);
	E_EXPECT_EQ(Settings.ResolveLayer(""), 0u);        // 비면 Default
	E_EXPECT_EQ(Settings.ResolveLayer("Missing"), 0u); // 없으면 Default
	E_EXPECT_EQ(Settings.ResolveLayerChecked("Missing"), 0u); // 경고 한 번
	E_EXPECT_EQ(Settings.ResolveLayerChecked("Missing"), 0u); // 다시 불러도 같은 값 (경고 없음)
	E_EXPECT_EQ(Settings.FindLayer("Missing"), -1);
}

E_TEST(SortingLayer_EditKeepsDefaultFirst)
{
	FSortingLayerSettings Settings;
	Settings.AddLayer("A");
	Settings.AddLayer("B");
	Settings.AddLayer("C");
	E_EXPECT_FALSE(Settings.MoveLayer(0, 2)); // Default 고정
	E_EXPECT_FALSE(Settings.MoveLayer(1, 0));
	E_EXPECT_TRUE(Settings.MoveLayer(3, 1)); // C를 맨 앞(Default 다음)으로 → Default, C, A, B
	E_EXPECT_EQ(Settings.ResolveLayer("C"), 1u);
	E_EXPECT_EQ(Settings.ResolveLayer("B"), 3u);
	E_EXPECT_FALSE(Settings.RenameLayer(0, "X"));
	E_EXPECT_FALSE(Settings.RenameLayer(1, "A")); // 중복
	E_EXPECT_TRUE(Settings.RenameLayer(1, "Front"));
	E_EXPECT_FALSE(Settings.RemoveLayer(0));
	E_EXPECT_TRUE(Settings.RemoveLayer(2)); // A
	E_EXPECT_EQ(Settings.ResolveLayer("A"), 0u);
	E_EXPECT_EQ(Settings.ResolveLayer("B"), 2u);
}

E_TEST(SortingLayer_JsonRoundTripAndRepair)
{
	FSortingLayerSettings Settings;
	Settings.AddLayer("Background");
	Settings.AddLayer("Foreground");
	FSortingLayerSettings Loaded;
	E_EXPECT_TRUE(Loaded.FromJson(Settings.ToJson()));
	E_EXPECT_TRUE(Loaded.GetLayerNames() == Settings.GetLayerNames());

	// 손으로 고친 파일: 첫 칸이 Default가 아님 + 중복 + 빈 이름 → 고쳐 읽고 경고 문구
	FSortingLayerSettings Repaired;
	std::string           Error;
	E_EXPECT_TRUE(Repaired.FromJson(R"({"Layers": ["Sky", "Ground", "Ground", ""]})", &Error));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_EQ(Repaired.GetLayerCount(), 3u);
	E_EXPECT_EQ(Repaired.GetLayerName(0), std::string("Default"));
	E_EXPECT_EQ(Repaired.GetLayerName(1), std::string("Sky"));
	E_EXPECT_EQ(Repaired.GetLayerName(2), std::string("Ground"));

	FSortingLayerSettings Broken;
	E_EXPECT_FALSE(Broken.FromJson("{ not json"));
}

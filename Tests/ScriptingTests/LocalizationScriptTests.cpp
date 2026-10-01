#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "UI/Localization.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"

#include <filesystem>
#include <fstream>

E_TEST(UIScript_LocalizationKeysAndLocTable)
{
	namespace fs           = std::filesystem;
	const fs::path Content = FTestRegistry::GetTempDirectory() / L"ProjectELocScriptTests";
	std::error_code ErrorCode;
	fs::remove_all(Content, ErrorCode);
	fs::create_directories(Content / L"Scripts");
	fs::create_directories(Content / L"UI");

	FLocalization& Loc = FLocalization::Get();
	Loc.ResetForTests();
	FStringTable Table;
	E_EXPECT_TRUE(Table.FromJsonString(R"({ "Languages": ["ko", "en"], "Strings": {
		"Title": { "ko": "제목", "en": "Title" },
		"Score": { "ko": "점수 {0}", "en": "Score {0}" },
		"Hello": { "ko": "{Name}님", "en": "Hi {Name}" } } })"));
	Loc.AddTable(Table);
	Loc.SetDefaultLanguage("ko");
	Loc.SetLanguage("ko");
	{
		FUIAsset Asset;
		FUIWidget* Title = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Text));
		Title->Name      = "Title";
		Title->TextKey   = "Title";
		FUIWidget* Score = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Text));
		Score->Name      = "Score";
		Score->TextKey   = "Title";
		E_EXPECT_TRUE(Asset.SaveToFile(Content / L"UI/Loc.eui"));
	}
	std::ofstream(Content / L"Scripts/Loc.lua", std::ios::binary | std::ios::trunc) << R"(
local Hud = { Properties = {} }
function Hud:OnStart()
	local Title = self.entity:GetWidget("Title")
	TitleKo  = Title.Text                       -- 키 반영
	TitleKey = Title.TextKey
	local Score = self.entity:GetWidget("Score")
	Score.Text = Loc.Get("Score", 7)             -- 고정 문자열을 쓰면 키가 떨어진다
	ScoreKey = Score.TextKey
	Hello = Loc.Get("Hello", { Name = "여우" })
	Langs = #Loc.GetLanguages()
	Bad = Loc.SetLanguage("fr", false)
	Ok = Loc.SetLanguage("en", false)
	Lang = Loc.GetLanguage()
	TitleEn = Title.Text
	Missing = Loc.Get("No.Key")
end
return Hud
)";

	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("Hud");
	Scene.GetRegistry().Emplace<FUIComponent>(Entity).Asset            = "UI/Loc.eui";
	Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Loc.lua";
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	FUIFrameInput Input;
	Input.Viewport = FUIRect(FVector2::ZeroVector, FVector2(800.0f, 600.0f));
	FUISystem::Update(Scene, Input, Content);
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString(R"(assert(TitleKo == "제목" and TitleKey == "Title" and ScoreKey == "", "keys"))"));
	E_EXPECT_TRUE(Scripts.RunString(R"(assert(Hello == "여우님" and Langs == 2 and Bad == false and Ok == true and Lang == "en", "loc"))"));
	E_EXPECT_TRUE(Scripts.RunString(R"(assert(TitleEn == "Title" and Missing == "No.Key", "switch"))"));
	const FUIInstance* Instance = Scene.GetRegistry().Get<FUIComponent>(Entity).Runtime.Instance.get();
	E_EXPECT_TRUE(Instance != nullptr && Instance->GetRoot().FindByName("Score")->Text == "점수 7");
	Scripts.EndPlay();
	Loc.ResetForTests();
}

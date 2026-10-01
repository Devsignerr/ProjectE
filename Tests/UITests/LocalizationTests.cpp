#include "Core/Testing/TestFramework.h"
#include "UI/Localization.h"
#include "UI/UIAsset.h"
#include "UI/UIFont.h"
#include "UI/UIInstance.h"
#include "UI/UIPainter.h"

namespace
{
	// ko/en 표 하나로 전역 다국어를 초기화한다 (기본 언어 ko, 현재 ko)
	void SetupKoreanEnglish()
	{
		FLocalization& Loc = FLocalization::Get();
		Loc.ResetForTests();
		FStringTable Table;
		E_EXPECT_TRUE(Table.FromJsonString(R"({
			"Version": 1,
			"Languages": ["ko", "en"],
			"Strings": {
				"Menu.Play":  { "ko": "게임 시작", "en": "Play" },
				"HUD.Score":  { "ko": "점수 {0}", "en": "Score {0}" },
				"HUD.Hello":  { "ko": "{Name}님 안녕하세요", "en": "Hello, {Name}" },
				"OnlyKorean": { "ko": "한국어만" },
				"Chat.Hint":  { "ko": "채팅 입력", "en": "Type to chat" }
			}
		})"));
		Loc.AddTable(Table);
		Loc.SetDefaultLanguage("ko");
		E_EXPECT_TRUE(Loc.SetLanguage("ko"));
	}
} // namespace

E_TEST(Localization_FormatPositionalNamedAndEscapes)
{
	FLocFormatArgs Args;
	Args.Positional = { "10", "20" };
	Args.Named      = { { "Name", "여우" } };
	E_EXPECT_EQ(FLocalization::Format("{0} / {1}", Args), std::string("10 / 20"));
	E_EXPECT_EQ(FLocalization::Format("{1}{0}{1}", Args), std::string("201020"));
	E_EXPECT_EQ(FLocalization::Format("안녕 {Name}!", Args), std::string("안녕 여우!"));
	E_EXPECT_EQ(FLocalization::Format("{{0}} = {0}", Args), std::string("{0} = 10"));
	// 없는 인자 / 닫히지 않은 괄호는 그대로
	E_EXPECT_EQ(FLocalization::Format("{2} {Missing} {", Args), std::string("{2} {Missing} {"));
	E_EXPECT_EQ(FLocalization::Format("", Args), std::string());
}

E_TEST(Localization_LanguageCodesAndMatching)
{
	E_EXPECT_EQ(FLocalization::NormalizeLanguage(" ko_KR "), std::string("ko-kr"));
	const std::vector<std::string> Available = { "ko", "en", "zh-cn" };
	E_EXPECT_EQ(FLocalization::MatchLanguage("ko-KR", Available), std::string("ko"));
	E_EXPECT_EQ(FLocalization::MatchLanguage("EN", Available), std::string("en"));
	E_EXPECT_EQ(FLocalization::MatchLanguage("zh-TW", Available), std::string("zh-cn")); // 앞부분이 같은 첫 언어
	E_EXPECT_EQ(FLocalization::MatchLanguage("fr", Available), std::string());
	E_EXPECT_EQ(FLocalization::GetLanguageDisplayName("ko"), std::string("한국어"));
	E_EXPECT_EQ(FLocalization::GetLanguageDisplayName("xx"), std::string("xx"));
}

E_TEST(Localization_StringTableJsonRoundTrip)
{
	FStringTable Table;
	Table.AddLanguage("ko");
	Table.AddLanguage("EN"); // 정규화
	E_EXPECT_FALSE(Table.AddLanguage("en"));
	Table.Strings["B.Key"]["ko"] = "나";
	Table.Strings["A.Key"]["ko"] = "가";
	Table.Strings["A.Key"]["en"] = "A";

	FStringTable Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Table.ToJsonString()));
	E_EXPECT_EQ(Loaded.Languages.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Loaded.Languages[1], std::string("en"));
	E_EXPECT_EQ(Loaded.Strings["A.Key"]["en"], std::string("A"));
	E_EXPECT_EQ(Loaded.Strings["B.Key"]["ko"], std::string("나"));
	E_EXPECT_TRUE(Loaded.Strings["B.Key"].find("en") == Loaded.Strings["B.Key"].end()); // 빈 번역은 저장하지 않는다

	// 목록에 없는 언어 값도 언어로 잡는다, 잘못된 JSON은 실패
	FStringTable Extra;
	E_EXPECT_TRUE(Extra.FromJsonString(R"({ "Languages": ["ko"], "Strings": { "K": { "ja": "日本" } } })"));
	E_EXPECT_EQ(Extra.Languages.size(), static_cast<size_t>(2));
	E_EXPECT_FALSE(Extra.FromJsonString("{ 잘못된"));
}

E_TEST(Localization_LookupFallbackAndLanguageSwitch)
{
	SetupKoreanEnglish();
	FLocalization& Loc = FLocalization::Get();
	E_EXPECT_EQ(Loc.GetLanguages().size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Loc.Lookup("Menu.Play"), std::string("게임 시작"));

	const uint32 RevisionBefore = Loc.GetRevision();
	E_EXPECT_TRUE(Loc.SetLanguage("en-US")); // 앞부분 일치
	E_EXPECT_EQ(Loc.GetLanguage(), std::string("en"));
	E_EXPECT_TRUE(Loc.GetRevision() != RevisionBefore);
	E_EXPECT_EQ(Loc.Lookup("Menu.Play"), std::string("Play"));
	E_EXPECT_EQ(Loc.Lookup("OnlyKorean"), std::string("한국어만")); // 현재 언어에 없으면 기본 언어
	E_EXPECT_EQ(Loc.Lookup("No.Such.Key"), std::string("No.Such.Key")); // 없는 키 = 키 문자열 (경고 한 번)
	E_EXPECT_EQ(Loc.Lookup("No.Such.Key"), std::string("No.Such.Key"));
	E_EXPECT_FALSE(Loc.Has("No.Such.Key"));
	E_EXPECT_TRUE(Loc.Find("OnlyKorean", "en") == nullptr);

	FLocFormatArgs Args;
	Args.Positional = { "120" };
	E_EXPECT_EQ(Loc.Get("HUD.Score", Args), std::string("Score 120"));
	Args.Named = { { "Name", "Fox" } };
	E_EXPECT_EQ(Loc.Get("HUD.Hello", Args), std::string("Hello, Fox"));

	E_EXPECT_FALSE(Loc.SetLanguage("fr")); // 표에 없는 언어는 그대로
	E_EXPECT_EQ(Loc.GetLanguage(), std::string("en"));
	Loc.ResetForTests();
}

E_TEST(Localization_WidgetKeysFollowLanguage)
{
	SetupKoreanEnglish();
	FUIAsset Asset;
	Asset.DesignSize       = FVector2(800.0f, 600.0f);
	Asset.ScaleMode        = EUIScaleMode::None;
	FUIWidget* Label       = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Text));
	Label->Name            = "Label";
	Label->Text            = "고정";
	Label->TextKey         = "Menu.Play";
	Label->Slot.bAutoSize  = true;
	FUIWidget* Box         = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::TextBox));
	Box->Name              = "Chat";
	Box->HintTextKey       = "Chat.Hint";
	Box->Slot.Offsets      = FUIMargin(0.0f, 100.0f, 300.0f, 40.0f);

	// JSON 왕복: 키는 저장, Text는 키가 없을 때의 값으로 남는다
	FUIAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	E_EXPECT_EQ(Loaded.Root->FindByName("Label")->TextKey, std::string("Menu.Play"));
	E_EXPECT_EQ(Loaded.Root->FindByName("Label")->Text, std::string("고정"));
	E_EXPECT_EQ(Loaded.Root->FindByName("Chat")->HintTextKey, std::string("Chat.Hint"));

	E_EXPECT_EQ(GetDisplayText(*Label), std::string("게임 시작"));
	E_EXPECT_EQ(GetDisplayHintText(*Box), std::string("채팅 입력"));
	E_EXPECT_EQ(GetDisplayText(*Box), std::string()); // 텍스트 상자 입력 내용은 번역하지 않는다

	// 인스턴스: 언어를 바꾸면 다음 레이아웃부터 크기/글자가 바뀐다 (자동 크기 텍스트)
	FUIInstance     Instance(Asset);
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	if (Fonts.GetDefaultFont() == nullptr)
	{
		return; // 글꼴 없는 환경
	}
	const FUIRect Viewport(FVector2::ZeroVector, FVector2(800.0f, 600.0f));
	Instance.Layout(Viewport, Fonts);
	const float KoreanWidth = Instance.FindWidget("Label")->State.Geometry.GetWidth();
	FLocalization::Get().SetLanguage("en");
	Instance.Layout(Viewport, Fonts);
	const float EnglishWidth = Instance.FindWidget("Label")->State.Geometry.GetWidth();
	E_EXPECT_TRUE(KoreanWidth > 0.0f && EnglishWidth > 0.0f && EnglishWidth != KoreanWidth);
	E_EXPECT_EQ(GetDisplayText(*Instance.FindWidget("Label")), std::string("Play"));

	// 그리기 목록: "Play" 4글자
	FUIDrawList List;
	FUIWidget   Single(EUIWidgetType::Text);
	Single.TextKey = "Menu.Play";
	FUIPainter::PaintText(Single, FUIRect(FVector2::ZeroVector, FVector2(400.0f, 40.0f)), 1.0f, FUITransform{}, FUIRect::Infinite(), Fonts, List);
	E_EXPECT_EQ(List.Quads.size(), static_cast<size_t>(4));

	// 키가 없으면 키 문자열을 그대로 보인다
	Single.TextKey = "Missing.Key";
	E_EXPECT_EQ(GetDisplayText(Single), std::string("Missing.Key"));
	FLocalization::Get().ResetForTests();
}

#include "Core/Input.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UILayout.h"
#include "UI/UIFont.h"
#include "UI/UISystem.h"

#include <algorithm>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	fs::path GetContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = FTestRegistry::GetTempDirectory() / L"ProjectEUISystemTests";
			std::error_code ErrorCode;
			fs::remove_all(Path, ErrorCode);
			fs::create_directories(Path / L"UI");
			return Path;
		}();
		return Directory;
	}

	// 캔버스 루트 + (X, Y, W, H) 버튼 하나. 설계 해상도 = 화면(1000x500), 배율 없음
	void WriteButtonUI(const char* File, const char* ButtonName, float X, float Y, float Width, float Height)
	{
		FUIAsset Asset;
		Asset.DesignSize     = FVector2(1000.0f, 500.0f);
		Asset.ScaleMode      = EUIScaleMode::None;
		FUIWidget* Button    = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Button));
		Button->Name         = ButtonName;
		Button->Slot.Offsets = FUIMargin(X, Y, Width, Height);
		E_EXPECT_TRUE(Asset.SaveToFile(GetContent() / File));
	}

	FUIFrameInput MakeInput(float X, float Y, bool bPressed = false)
	{
		FUIFrameInput Input;
		Input.Viewport         = FUIRect(FVector2::ZeroVector, FVector2(1000.0f, 500.0f));
		Input.bHasPointer      = true;
		Input.Pointer.bInside  = true;
		Input.Pointer.Position = FVector2(X, Y);
		Input.Pointer.bPressed = bPressed;
		Input.Pointer.bDown    = bPressed;
		return Input;
	}

	bool HasEvent(const FUIComponent& Component, EUIEventType Type, const char* Name)
	{
		for (const FUIEvent& Event : Component.Runtime.Events)
		{
			if (Event.Type == Type && Event.WidgetName == Name)
			{
				return true;
			}
		}
		return false;
	}
} // namespace

E_TEST(UISystem_TopUIGetsPointerFirst)
{
	WriteButtonUI("UI/Bottom.eui", "Big", 0.0f, 0.0f, 1000.0f, 500.0f);
	WriteButtonUI("UI/Top.eui", "Small", 0.0f, 0.0f, 100.0f, 100.0f);

	FScene        Scene;
	const FEntity BottomEntity = Scene.CreateEntity("Bottom");
	const FEntity TopEntity    = Scene.CreateEntity("Top");
	Scene.GetRegistry().Emplace<FUIComponent>(BottomEntity).Asset = "UI/Bottom.eui";
	FUIComponent& TopComponent  = Scene.GetRegistry().Emplace<FUIComponent>(TopEntity);
	TopComponent.Asset          = "UI/Top.eui";
	TopComponent.ZOrder         = 5;

	// 두 UI가 겹친 곳: 위(ZOrder 5)만 호버
	E_EXPECT_TRUE(FUISystem::Update(Scene, MakeInput(50.0f, 50.0f), GetContent()).bPointer);
	const FUIComponent& Top    = Scene.GetRegistry().Get<FUIComponent>(TopEntity);
	const FUIComponent& Bottom = Scene.GetRegistry().Get<FUIComponent>(BottomEntity);
	E_EXPECT_TRUE(HasEvent(Top, EUIEventType::HoverBegin, "Small"));
	E_EXPECT_TRUE(Bottom.Runtime.Events.empty());
	E_EXPECT_TRUE(Top.Runtime.bPointerOver && !Bottom.Runtime.bPointerOver);

	// 위 UI 밖: 아래 UI가 받는다 (이전 프레임 이벤트는 비워진다)
	E_EXPECT_TRUE(FUISystem::Update(Scene, MakeInput(500.0f, 300.0f), GetContent()).bPointer);
	E_EXPECT_TRUE(HasEvent(Scene.GetRegistry().Get<FUIComponent>(BottomEntity), EUIEventType::HoverBegin, "Big"));
	E_EXPECT_TRUE(HasEvent(Scene.GetRegistry().Get<FUIComponent>(TopEntity), EUIEventType::HoverEnd, "Small"));

	// 입력 끔 / 숨김: 포인터를 가져가지 않는다, 숨기면 그리지도 않는다
	Scene.GetRegistry().Get<FUIComponent>(BottomEntity).bReceiveInput = false;
	E_EXPECT_FALSE(FUISystem::Update(Scene, MakeInput(500.0f, 300.0f), GetContent()).bPointer);
	FUIDrawList List;
	FUISystem::Paint(Scene, List);
	E_EXPECT_EQ(List.Quads.size(), size_t(2));
	Scene.GetRegistry().Get<FUIComponent>(TopEntity).bVisible = false;
	List.Clear();
	FUISystem::Update(Scene, MakeInput(50.0f, 50.0f), GetContent());
	FUISystem::Paint(Scene, List);
	E_EXPECT_EQ(List.Quads.size(), size_t(1));
}

E_TEST(UISystem_CopyDoesNotShareRuntimeAndAssetReloads)
{
	WriteButtonUI("UI/Reload.eui", "First", 0.0f, 0.0f, 10.0f, 10.0f);
	FUIComponent Component;
	Component.Asset       = "UI/Reload.eui";
	FUIInstance* Instance = FUISystem::EnsureInstance(Component, GetContent());
	E_EXPECT_TRUE(Instance != nullptr && Instance->FindWidget("First") != nullptr);

	// 플레이 모드 복제처럼 값 복사 → 런타임은 비어 있다 (인스턴스를 공유하지 않음)
	const FUIComponent Copy = Component;
	E_EXPECT_TRUE(Copy.Runtime.Instance == nullptr && Copy.Asset == Component.Asset);

	// 파일이 바뀌면(수정 시각) 새 인스턴스부터 반영
	WriteButtonUI("UI/Reload.eui", "Second", 0.0f, 0.0f, 10.0f, 10.0f);
	std::filesystem::last_write_time(GetContent() / L"UI/Reload.eui", std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
	FUIComponent Fresh;
	Fresh.Asset = "UI/Reload.eui";
	FUIInstance* Reloaded = FUISystem::EnsureInstance(Fresh, GetContent());
	E_EXPECT_TRUE(Reloaded != nullptr && Reloaded->FindWidget("Second") != nullptr);

	// 없는 에셋은 nullptr
	FUIComponent Missing;
	Missing.Asset = "UI/Nope.eui";
	E_EXPECT_TRUE(FUISystem::EnsureInstance(Missing, GetContent()) == nullptr);
}

E_TEST(UISystem_AnimationTicksWithoutInput)
{
	// 입력을 받지 않는 UI(HUD)도 UI 애니메이션이 진행되고 끝 이벤트가 난다
	FUIAsset Asset;
	Asset.DesignSize   = FVector2(1000.0f, 500.0f);
	Asset.ScaleMode    = EUIScaleMode::None;
	FUIWidget* Panel   = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
	Panel->Name        = "Panel";
	FUIAnimation& Fade = Asset.Animations.emplace_back();
	Fade.Name          = "FadeIn";
	Fade.Length        = 0.5f;
	Fade.GetOrAddTrack("Panel", EUIAnimProperty::Opacity).Keys = { { 0.0f, 0.0f }, { 0.5f, 1.0f } };
	E_EXPECT_TRUE(Asset.SaveToFile(GetContent() / L"UI/Hud.eui"));

	FScene        Scene;
	const FEntity Entity     = Scene.CreateEntity("Hud");
	FUIComponent& Component  = Scene.GetRegistry().Emplace<FUIComponent>(Entity);
	Component.Asset          = "UI/Hud.eui";
	Component.bReceiveInput  = false;
	FUIInstance* Instance    = FUISystem::EnsureInstance(Component, GetContent());
	E_EXPECT_TRUE(Instance != nullptr && Instance->PlayAnimation("FadeIn"));
	FUIFrameInput Input      = MakeInput(0.0f, 0.0f);
	Input.DeltaSeconds       = 0.25f;
	FUISystem::Update(Scene, Input, GetContent());
	const FUIWidget* Widget  = Instance->FindWidget("Panel");
	E_EXPECT_TRUE(Widget != nullptr && Widget->RenderOpacity > 0.4f && Widget->RenderOpacity < 0.6f);
	FUISystem::Update(Scene, Input, GetContent());
	E_EXPECT_NEAR(Widget->RenderOpacity, 1.0f, 1.0e-4f);
	E_EXPECT_TRUE(HasEvent(Scene.GetRegistry().Get<FUIComponent>(Entity), EUIEventType::AnimationFinished, "FadeIn"));
}

E_TEST(UISystem_InputWithoutMouseButtons)
{
	FInput       Input;
	FWindowEvent Event;
	Event.Type   = EWindowEventType::MouseButtonDown;
	Event.Button = EMouseButton::Left;
	Event.MouseX = 30;
	Event.MouseY = 40;
	Input.ProcessEvent(Event);
	FWindowEvent Key;
	Key.Type = EWindowEventType::KeyDown;
	Key.Key  = EKey::W;
	Input.ProcessEvent(Key);
	E_EXPECT_TRUE(Input.IsMouseButtonPressed(EMouseButton::Left));

	const FInput Blocked = Input.WithoutMouseButtons();
	E_EXPECT_FALSE(Blocked.IsMouseButtonDown(EMouseButton::Left));
	E_EXPECT_FALSE(Blocked.IsMouseButtonPressed(EMouseButton::Left));
	E_EXPECT_TRUE(Blocked.IsKeyDown(EKey::W)); // 키보드는 그대로
	E_EXPECT_EQ(Blocked.GetMouseX(), 30);

	const FUIPointerInput Pointer = FUISystem::MakePointer(Input, FVector2(-10.0f, -20.0f), true);
	E_EXPECT_NEAR(Pointer.Position.X, 20.0f, 0.0f);
	E_EXPECT_NEAR(Pointer.Position.Y, 20.0f, 0.0f);
	E_EXPECT_TRUE(Pointer.bPressed && Pointer.bDown);
}

E_TEST(UITextBox_EditCommitAndKeyboardCapture)
{
	auto       Root = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Box  = Root->AddChild(FUIWidget::Create(EUIWidgetType::TextBox));
	Box->Name         = "Name";
	Box->Slot.Offsets = FUIMargin(0.0f, 0.0f, 300.0f, 40.0f);
	Box->MaxLength    = 5;
	Root->AssignIds();
	FUILayout::Compute(*Root, FVector2(800.0f, 600.0f), FUIFontLibrary::Get());

	FUIInputRouter        Router;
	std::vector<FUIEvent> Events;
	const auto HasEvent = [&Events](EUIEventType Type) {
		return std::any_of(Events.begin(), Events.end(), [Type](const FUIEvent& Event) { return Event.Type == Type && Event.WidgetName == "Name"; });
	};

	// 클릭 → 포커스 (키보드를 가져감)
	FUIPointerInput Pointer;
	Pointer.bInside  = true;
	Pointer.Position = FVector2(20.0f, 20.0f);
	Pointer.bPressed = Pointer.bDown = true;
	E_EXPECT_TRUE(Router.Process(*Root, Pointer, {}, Events));
	E_EXPECT_TRUE(Router.WantsKeyboard(*Root) && Box->State.bFocused);
	Pointer = FUIPointerInput{};

	// 입력 "가나" → 왼쪽 → "a" 삽입 → "가a나"
	FUIKeyInput Keys;
	Keys.Typed = U"가나";
	Events.clear();
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_TRUE(HasEvent(EUIEventType::TextChanged));
	Keys       = {};
	Keys.bLeft = true;
	Router.Process(*Root, Pointer, Keys, Events);
	Keys       = {};
	Keys.Typed = U"a";
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->Text, std::string("가a나"));
	E_EXPECT_EQ(Box->State.CaretIndex, 2);

	// 지우기 / Delete / Home / 최대 길이
	Keys            = {};
	Keys.bBackspace = true;
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->Text, std::string("가나"));
	Keys       = {};
	Keys.bHome = true;
	Router.Process(*Root, Pointer, Keys, Events);
	Keys         = {};
	Keys.bDelete = true;
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->Text, std::string("나"));
	Keys       = {};
	Keys.Typed = U"123456789";
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->Text, std::string("1234나")); // 최대 5자: 캐럿(맨 앞)에 4자만 들어간다
	E_EXPECT_EQ(DecodeUtf8(Box->Text).size(), size_t(5));

	// Enter → 확정 + 포커스 해제 (게임에 키보드 반환)
	Keys         = {};
	Keys.bCommit = true;
	Events.clear();
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_TRUE(HasEvent(EUIEventType::TextCommitted));
	E_EXPECT_FALSE(Router.WantsKeyboard(*Root));

	// 다시 포커스 후 빈 곳 클릭 → 확정 이벤트 + 해제, Esc는 확정 없이 해제
	Pointer.bInside  = true;
	Pointer.Position = FVector2(20.0f, 20.0f);
	Pointer.bPressed = Pointer.bDown = true;
	Router.Process(*Root, Pointer, {}, Events);
	Pointer.Position = FVector2(500.0f, 500.0f);
	Events.clear();
	Router.Process(*Root, Pointer, {}, Events);
	E_EXPECT_TRUE(HasEvent(EUIEventType::TextCommitted) && !Router.WantsKeyboard(*Root));
	Pointer.Position = FVector2(20.0f, 20.0f);
	Router.Process(*Root, Pointer, {}, Events);
	Pointer          = FUIPointerInput{};
	Keys             = {};
	Keys.bCancel     = true;
	Events.clear();
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_FALSE(HasEvent(EUIEventType::TextCommitted));
	E_EXPECT_FALSE(Router.WantsKeyboard(*Root));

	// 클릭 위치 → 캐럿: 맨 왼쪽은 0, 맨 오른쪽 밖은 끝
	Pointer.bInside  = true;
	Pointer.bPressed = Pointer.bDown = true;
	Pointer.Position = FVector2(1.0f, 20.0f);
	Router.Process(*Root, Pointer, {}, Events);
	E_EXPECT_EQ(Box->State.CaretIndex, 0);
	Pointer.Position = FVector2(290.0f, 20.0f);
	Router.Process(*Root, Pointer, {}, Events);
	E_EXPECT_EQ(Box->State.CaretIndex, 5);

	// JSON 왕복
	FUIAsset Asset;
	Asset.Root->AddChild(Box->Clone());
	FUIAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	const FUIWidget* LoadedBox = Loaded.Root->FindByName("Name");
	E_EXPECT_TRUE(LoadedBox != nullptr && LoadedBox->Type == EUIWidgetType::TextBox && LoadedBox->MaxLength == 5 && LoadedBox->HintText == Box->HintText);
}

E_TEST(UITextBox_InputTypedTextAndKeyboardBlock)
{
	FInput       Input;
	FWindowEvent Char;
	Char.Type      = EWindowEventType::Char;
	Char.Character = 0xD55C; // 한
	Input.ProcessEvent(Char);
	Char.Character = 0x08; // Backspace 문자 (키 이벤트로 따로 처리)
	Input.ProcessEvent(Char);
	FWindowEvent Key;
	Key.Type    = EWindowEventType::KeyDown;
	Key.Key     = EKey::Backspace;
	Key.bRepeat = true;
	Input.ProcessEvent(Key);
	E_EXPECT_EQ(Input.GetTypedText().size(), size_t(2));
	E_EXPECT_TRUE(Input.IsKeyRepeated(EKey::Backspace));

	const FUIKeyInput Keys = FUISystem::MakeKeys(Input);
	E_EXPECT_TRUE(Keys.Typed == std::u32string(U"한")); // 제어 문자 제외
	E_EXPECT_TRUE(Keys.bBackspace);

	const FInput Blocked = Input.WithoutKeyboard();
	E_EXPECT_TRUE(Blocked.GetTypedText().empty());
	E_EXPECT_FALSE(Blocked.IsKeyDown(EKey::Backspace));

	Input.EndFrame(); // 다음 프레임: 문자/반복은 비워지고 누름 상태는 유지
	E_EXPECT_TRUE(Input.GetTypedText().empty() && !Input.IsKeyRepeated(EKey::Backspace) && Input.IsKeyDown(EKey::Backspace));
}

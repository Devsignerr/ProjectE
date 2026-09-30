#include "Core/Testing/TestFramework.h"
#include "UI/UIAsset.h"
#include "UI/UIFont.h"
#include "UI/UIInput.h"
#include "UI/UIInstance.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"
#include "UI/Widget.h"

#include <filesystem>

namespace
{
	// 글자당 FontSize × 0.5 너비, 줄 높이 = FontSize. 줄바꿈은 너비를 넘는 만큼 줄 수를 늘린다
	class FFakeMeasurer final : public IUITextMeasurer
	{
	public:
		FVector2 MeasureText(const FUIWidgetData& Widget, float WrapWidth) override
		{
			const float Width = static_cast<float>(Widget.Text.size()) * Widget.FontSize * 0.5f;
			if (WrapWidth > 0.0f && Width > WrapWidth)
			{
				const float Lines = FMath::Ceil(Width / WrapWidth);
				return FVector2(WrapWidth, Lines * Widget.FontSize);
			}
			return FVector2(Width, Widget.FontSize);
		}
	};

	bool RectNear(const FUIRect& Rect, float MinX, float MinY, float MaxX, float MaxY, float Tolerance = 0.01f)
	{
		return FMath::IsNearlyEqual(Rect.Min.X, MinX, Tolerance) && FMath::IsNearlyEqual(Rect.Min.Y, MinY, Tolerance) &&
		       FMath::IsNearlyEqual(Rect.Max.X, MaxX, Tolerance) && FMath::IsNearlyEqual(Rect.Max.Y, MaxY, Tolerance);
	}

	FUIWidget* AddChild(FUIWidget& Parent, EUIWidgetType Type, const char* Name)
	{
		FUIWidget* Child = Parent.AddChild(FUIWidget::Create(Type));
		Child->Name      = Name;
		return Child;
	}

	FUIWidget* AddImage(FUIWidget& Parent, const char* Name, float Width, float Height)
	{
		FUIWidget* Image = AddChild(Parent, EUIWidgetType::Image, Name);
		Image->ImageSize = FVector2(Width, Height);
		return Image;
	}

	void Layout(FUIWidget& Root, float Width, float Height)
	{
		FFakeMeasurer Measurer;
		Root.AssignIds();
		FUILayout::Compute(Root, FVector2(Width, Height), Measurer);
	}
} // namespace

// ---------------------------------------------------------------- 레이아웃

E_TEST(UILayout_CanvasPointAnchorWithAlignment)
{
	auto       Root  = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Child = AddImage(*Root, "Center", 10.0f, 10.0f);
	// 화면 가운데 앵커, 피벗 가운데, 200x100
	Child->Slot.AnchorMin = Child->Slot.AnchorMax = FVector2(0.5f, 0.5f);
	Child->Slot.Alignment                         = FVector2(0.5f, 0.5f);
	Child->Slot.Offsets                           = FUIMargin(0.0f, 0.0f, 200.0f, 100.0f);
	Layout(*Root, 1000.0f, 600.0f);
	E_EXPECT_TRUE(RectNear(Child->State.Geometry, 400.0f, 250.0f, 600.0f, 350.0f));

	// 오른쪽 아래 앵커 + 음수 위치 + 자동 크기
	Child->Slot.AnchorMin = Child->Slot.AnchorMax = FVector2(1.0f, 1.0f);
	Child->Slot.Alignment                         = FVector2(1.0f, 1.0f);
	Child->Slot.Offsets                           = FUIMargin(-20.0f, -30.0f, 0.0f, 0.0f);
	Child->Slot.bAutoSize                         = true;
	Layout(*Root, 1000.0f, 600.0f);
	E_EXPECT_TRUE(RectNear(Child->State.Geometry, 970.0f, 560.0f, 980.0f, 570.0f));
}

E_TEST(UILayout_CanvasStretchAnchor)
{
	auto       Root  = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Child = AddChild(*Root, EUIWidgetType::Border, "Bar");
	// 위쪽 가로 늘이기: 좌우 여백 10, 위 5, 높이 40
	Child->Slot.AnchorMin = FVector2(0.0f, 0.0f);
	Child->Slot.AnchorMax = FVector2(1.0f, 0.0f);
	Child->Slot.Offsets   = FUIMargin(10.0f, 5.0f, 10.0f, 40.0f);
	Layout(*Root, 800.0f, 600.0f);
	E_EXPECT_TRUE(RectNear(Child->State.Geometry, 10.0f, 5.0f, 790.0f, 45.0f));

	// 전체 늘이기
	Child->Slot.AnchorMax = FVector2(1.0f, 1.0f);
	Child->Slot.Offsets   = FUIMargin(10.0f, 20.0f, 30.0f, 40.0f);
	Layout(*Root, 800.0f, 600.0f);
	E_EXPECT_TRUE(RectNear(Child->State.Geometry, 10.0f, 20.0f, 770.0f, 560.0f));
}

E_TEST(UILayout_HorizontalBoxAutoAndFill)
{
	auto Root = FUIWidget::Create(EUIWidgetType::HorizontalBox);
	FUIWidget* A = AddImage(*Root, "A", 100.0f, 20.0f);
	FUIWidget* B = AddImage(*Root, "B", 10.0f, 50.0f);
	FUIWidget* C = AddImage(*Root, "C", 10.0f, 10.0f);
	A->Slot.Padding  = FUIMargin(5.0f, 0.0f);
	B->Slot.SizeRule = EUISizeRule::Fill;
	C->Slot.SizeRule = EUISizeRule::Fill;
	C->Slot.FillWeight = 3.0f;
	C->Slot.VAlign   = EUIVAlign::Center;
	Layout(*Root, 510.0f, 100.0f);
	// 남은 폭 = 510 - 110 = 400 → B 100, C 300
	E_EXPECT_TRUE(RectNear(A->State.Geometry, 5.0f, 0.0f, 105.0f, 100.0f));
	E_EXPECT_TRUE(RectNear(B->State.Geometry, 110.0f, 0.0f, 210.0f, 100.0f));
	E_EXPECT_TRUE(RectNear(C->State.Geometry, 210.0f, 45.0f, 510.0f, 55.0f));
	// 원하는 크기: 폭 합 (A 110 + B 10 + C 10), 높이 최대
	E_EXPECT_NEAR(Root->State.DesiredSize.X, 130.0f, 0.01f);
	E_EXPECT_NEAR(Root->State.DesiredSize.Y, 50.0f, 0.01f);
}

E_TEST(UILayout_VerticalBoxAlignmentAndCollapsed)
{
	auto       Root   = FUIWidget::Create(EUIWidgetType::VerticalBox);
	FUIWidget* Top    = AddImage(*Root, "Top", 40.0f, 20.0f);
	FUIWidget* Hidden = AddImage(*Root, "Hidden", 40.0f, 30.0f);
	FUIWidget* Gone   = AddImage(*Root, "Gone", 40.0f, 1000.0f);
	FUIWidget* Last   = AddImage(*Root, "Last", 40.0f, 10.0f);
	Top->Slot.HAlign   = EUIHAlign::Right;
	Hidden->Visibility = EUIVisibility::Hidden;    // 공간 차지
	Gone->Visibility   = EUIVisibility::Collapsed; // 공간 없음
	Last->Slot.HAlign  = EUIHAlign::Center;
	Layout(*Root, 200.0f, 500.0f);
	E_EXPECT_TRUE(RectNear(Top->State.Geometry, 160.0f, 0.0f, 200.0f, 20.0f));
	E_EXPECT_TRUE(RectNear(Last->State.Geometry, 80.0f, 50.0f, 120.0f, 60.0f));
	E_EXPECT_NEAR(Root->State.DesiredSize.Y, 60.0f, 0.01f);
}

E_TEST(UILayout_WrappedTextInVerticalBox)
{
	auto       Root  = FUIWidget::Create(EUIWidgetType::VerticalBox);
	FUIWidget* Text  = AddChild(*Root, EUIWidgetType::Text, "Text");
	FUIWidget* After = AddImage(*Root, "After", 10.0f, 10.0f);
	Text->Text     = "0123456789"; // 가짜 측정: 10자 × 10 = 100 폭
	Text->FontSize = 20.0f;
	Text->bWrap    = true;
	Layout(*Root, 40.0f, 500.0f);
	// 폭 40 → 3줄 = 60 높이
	E_EXPECT_NEAR(Text->State.Geometry.GetHeight(), 60.0f, 0.01f);
	E_EXPECT_NEAR(After->State.Geometry.Min.Y, 60.0f, 0.01f);
}

E_TEST(UILayout_OverlayBorderAndGrid)
{
	auto       Root    = FUIWidget::Create(EUIWidgetType::Overlay);
	FUIWidget* Border  = AddChild(*Root, EUIWidgetType::Border, "Border");
	Border->Slot.HAlign   = EUIHAlign::Center;
	Border->Slot.VAlign   = EUIVAlign::Center;
	Border->ContentPadding = FUIMargin(10.0f);
	FUIWidget* Grid = AddChild(*Border, EUIWidgetType::UniformGrid, "Grid");
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FUIWidget* Cell   = AddImage(*Grid, "Cell", 20.0f, 10.0f);
		Cell->Slot.Row    = Index / 2;
		Cell->Slot.Column = Index % 2;
		Cell->Slot.Padding = FUIMargin(2.0f);
	}
	Layout(*Root, 300.0f, 200.0f);
	// 칸 = (24, 14), 2x2 → 48x28, 보더 여백 10 → 68x48, 가운데
	E_EXPECT_TRUE(RectNear(Border->State.Geometry, 116.0f, 76.0f, 184.0f, 124.0f));
	E_EXPECT_TRUE(RectNear(Grid->State.Geometry, 126.0f, 86.0f, 174.0f, 114.0f));
	// 오른쪽 아래 칸 (Fill 정렬 → 칸에서 여백만 뺌)
	E_EXPECT_TRUE(RectNear(Grid->Children[3]->State.Geometry, 152.0f, 102.0f, 172.0f, 112.0f));
}

E_TEST(UILayout_ScrollBoxClampsAndClips)
{
	auto       Root   = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Scroll = AddChild(*Root, EUIWidgetType::ScrollBox, "Scroll");
	Scroll->Slot.Offsets  = FUIMargin(0.0f, 0.0f, 100.0f, 100.0f);
	Scroll->ScrollbarWidth = 10.0f;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		AddImage(*Scroll, "Row", 50.0f, 50.0f);
	}
	Scroll->State.ScrollOffset = 1000.0f; // 최대로 잘린다
	Layout(*Root, 800.0f, 600.0f);
	E_EXPECT_NEAR(Scroll->State.ScrollMax, 150.0f, 0.01f);
	E_EXPECT_NEAR(Scroll->State.ScrollOffset, 150.0f, 0.01f);
	// 첫 줄은 위로 150 밀림, 폭은 스크롤 막대만큼 줄어듦
	E_EXPECT_TRUE(RectNear(Scroll->Children[0]->State.Geometry, 0.0f, -150.0f, 90.0f, -100.0f));
	E_EXPECT_TRUE(RectNear(Scroll->Children[0]->State.Clip, 0.0f, 0.0f, 100.0f, 100.0f));
}

E_TEST(UILayout_ScaleModes)
{
	const FVector2 Design(1920.0f, 1080.0f);
	const FVector2 Viewport(1280.0f, 1024.0f);
	E_EXPECT_NEAR(FUILayout::ComputeScale(EUIScaleMode::None, Design, Viewport), 1.0f, 1e-5f);
	E_EXPECT_NEAR(FUILayout::ComputeScale(EUIScaleMode::MatchHeight, Design, Viewport), 1024.0f / 1080.0f, 1e-5f);
	E_EXPECT_NEAR(FUILayout::ComputeScale(EUIScaleMode::MatchWidth, Design, Viewport), 1280.0f / 1920.0f, 1e-5f);
	E_EXPECT_NEAR(FUILayout::ComputeScale(EUIScaleMode::Fit, Design, Viewport), 1280.0f / 1920.0f, 1e-5f);
	E_EXPECT_NEAR(FUILayout::ComputeScale(EUIScaleMode::Fill, Design, Viewport), 1024.0f / 1080.0f, 1e-5f);
}

E_TEST(UILayout_CanvasSlotEditingKeepsRect)
{
	const FUIRect Parent(FVector2(0.0f, 0.0f), FVector2(1000.0f, 500.0f));
	FUISlot       Slot;
	Slot.AnchorMin = Slot.AnchorMax = FVector2(0.5f, 1.0f);
	Slot.Alignment                  = FVector2(0.5f, 1.0f);
	Slot.Offsets                    = FUIMargin(0.0f, -10.0f, 200.0f, 50.0f);
	const FUIRect Before            = FUILayout::ArrangeCanvasSlot(Parent, Slot, FVector2::ZeroVector);
	E_EXPECT_TRUE(RectNear(Before, 400.0f, 440.0f, 600.0f, 490.0f));

	FUILayout::MoveCanvasSlot(Slot, FVector2(15.0f, -5.0f));
	E_EXPECT_TRUE(RectNear(FUILayout::ArrangeCanvasSlot(Parent, Slot, FVector2::ZeroVector), 415.0f, 435.0f, 615.0f, 485.0f));

	// 새 영역 지정 → 같은 영역이 다시 나온다
	const FUIRect Target(FVector2(100.0f, 50.0f), FVector2(300.0f, 90.0f));
	FUILayout::SetCanvasSlotRect(Slot, Parent, Target);
	E_EXPECT_TRUE(RectNear(FUILayout::ArrangeCanvasSlot(Parent, Slot, FVector2::ZeroVector), 100.0f, 50.0f, 300.0f, 90.0f));

	// 앵커를 전체 늘이기로 바꿔도 화면상 영역 유지
	FUILayout::SetAnchorsKeepRect(Slot, Parent, Target, FVector2(0.0f, 0.0f), FVector2(1.0f, 1.0f));
	E_EXPECT_TRUE(RectNear(FUILayout::ArrangeCanvasSlot(Parent, Slot, FVector2::ZeroVector), 100.0f, 50.0f, 300.0f, 90.0f));
	E_EXPECT_NEAR(Slot.Offsets.Right, 700.0f, 0.01f);
	E_EXPECT_NEAR(Slot.Offsets.Bottom, 410.0f, 0.01f);
	// 늘이기 축 이동은 양쪽 여백을 함께
	FUILayout::MoveCanvasSlot(Slot, FVector2(10.0f, 0.0f));
	E_EXPECT_TRUE(RectNear(FUILayout::ArrangeCanvasSlot(Parent, Slot, FVector2::ZeroVector), 110.0f, 50.0f, 310.0f, 90.0f));
}

// ---------------------------------------------------------------- 에셋

E_TEST(UIAsset_JsonRoundTrip)
{
	FUIAsset Asset;
	Asset.DesignSize = FVector2(1280.0f, 720.0f);
	Asset.ScaleMode  = EUIScaleMode::Fit;
	FUIWidget* Box   = AddChild(*Asset.Root, EUIWidgetType::VerticalBox, "Menu");
	Box->Slot.AnchorMin = FVector2(0.5f, 0.5f);
	Box->Slot.AnchorMax = FVector2(0.5f, 0.5f);
	Box->Slot.ZOrder    = 3;
	FUIWidget* Button   = AddChild(*Box, EUIWidgetType::Button, "Play");
	Button->Slot.SizeRule = EUISizeRule::Fill;
	Button->Slot.Padding  = FUIMargin(4.0f);
	Button->HoveredBrush.Texture = "UI/Hover.png";
	FUIWidget* Label = AddChild(*Button, EUIWidgetType::Text, "PlayLabel");
	Label->Text       = "시작하기";
	Label->Justify    = EUITextJustify::Center;
	Label->ShadowOffset = FVector2(1.0f, 2.0f);
	FUIWidget* Bar     = AddChild(*Asset.Root, EUIWidgetType::ProgressBar, "Health");
	Bar->Percent       = 0.25f;
	Bar->FillDirection = EUIFillDirection::RightToLeft;
	Bar->bEnabled      = false;

	const std::string Json = Asset.ToJsonString();
	FUIAsset          Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Json));
	E_EXPECT_EQ(Loaded.ToJsonString(), Json);
	E_EXPECT_NEAR(Loaded.DesignSize.X, 1280.0f, 0.0f);
	E_EXPECT_TRUE(Loaded.ScaleMode == EUIScaleMode::Fit);

	const FUIWidget* LoadedLabel = Loaded.Root->FindByName("PlayLabel");
	E_EXPECT_TRUE(LoadedLabel != nullptr);
	if (LoadedLabel != nullptr)
	{
		E_EXPECT_EQ(LoadedLabel->Text, std::string("시작하기"));
		E_EXPECT_TRUE(LoadedLabel->Parent != nullptr && LoadedLabel->Parent->Name == "Play");
		E_EXPECT_TRUE(LoadedLabel->Parent->Slot.SizeRule == EUISizeRule::Fill);
		E_EXPECT_EQ(LoadedLabel->Parent->HoveredBrush.Texture, std::string("UI/Hover.png"));
	}
	const FUIWidget* LoadedBar = Loaded.Root->FindByName("Health");
	E_EXPECT_TRUE(LoadedBar != nullptr && !LoadedBar->bEnabled && LoadedBar->FillDirection == EUIFillDirection::RightToLeft);
	E_EXPECT_TRUE(Loaded.Root->FindByName("Menu")->Slot.ZOrder == 3);

	// 하위 트리 복사/붙여넣기
	const std::unique_ptr<FUIWidget> Pasted = FUIAsset::WidgetFromJsonString(FUIAsset::WidgetToJsonString(*Button));
	E_EXPECT_TRUE(Pasted != nullptr && Pasted->Children.size() == 1 && Pasted->Slot.SizeRule == EUISizeRule::Fill);
}

E_TEST(UIAsset_RejectsTooManyChildrenAndUnknownTypes)
{
	const char* Json = R"({ "Version": 1, "Root": { "Type": "Button", "Name": "B", "Children": [
		{ "Type": "Text", "Name": "One" }, { "Type": "Text", "Name": "Two" }, { "Type": "Wat" } ] } })";
	FUIAsset Asset;
	E_EXPECT_TRUE(Asset.FromJsonString(Json));
	E_EXPECT_EQ(Asset.Root->Children.size(), size_t(1));
	E_EXPECT_TRUE(Asset.Root->FindByName("Two") == nullptr);
	E_EXPECT_FALSE(Asset.FromJsonString("{ 잘못된"));
}

// ---------------------------------------------------------------- 입력

E_TEST(UIInput_HitTestVisibilityAndZOrder)
{
	auto       Root  = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Back  = AddImage(*Root, "Back", 0.0f, 0.0f);
	FUIWidget* Front = AddImage(*Root, "Front", 0.0f, 0.0f);
	Back->Slot.Offsets  = FUIMargin(0.0f, 0.0f, 100.0f, 100.0f);
	Front->Slot.Offsets = FUIMargin(50.0f, 50.0f, 100.0f, 100.0f);
	Back->Slot.ZOrder   = 5; // 나중에 있어도 위
	Layout(*Root, 400.0f, 400.0f);
	E_EXPECT_TRUE(FUIInputRouter::HitTest(*Root, FVector2(75.0f, 75.0f)) == Back);
	E_EXPECT_TRUE(FUIInputRouter::HitTest(*Root, FVector2(125.0f, 125.0f)) == Front);
	// 캔버스 자체는 SelfHitTestInvisible → 빈 곳은 맞히지 않음 (게임으로 전달)
	E_EXPECT_TRUE(FUIInputRouter::HitTest(*Root, FVector2(300.0f, 300.0f)) == nullptr);
	Back->Visibility = EUIVisibility::HitTestInvisible;
	E_EXPECT_TRUE(FUIInputRouter::HitTest(*Root, FVector2(75.0f, 75.0f)) == Front);
}

E_TEST(UIInput_ClickHoverAndDisabled)
{
	auto       Root   = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Button = AddChild(*Root, EUIWidgetType::Button, "Ok");
	FUIWidget* Label  = AddChild(*Button, EUIWidgetType::Text, "Label");
	Button->Slot.Offsets = FUIMargin(10.0f, 10.0f, 100.0f, 40.0f);
	Label->Text          = "OK";
	Layout(*Root, 400.0f, 400.0f);

	FUIInputRouter        Router;
	std::vector<FUIEvent> Events;
	FUIPointerInput       Pointer;
	Pointer.bInside  = true;
	Pointer.Position = FVector2(50.0f, 20.0f); // 글자 위 → 버튼 조상으로 전달
	E_EXPECT_TRUE(Router.Process(*Root, Pointer, {}, Events));
	E_EXPECT_TRUE(Button->State.bHovered);
	E_EXPECT_TRUE(Events.size() == 1 && Events[0].Type == EUIEventType::HoverBegin && Events[0].WidgetName == "Ok");

	Events.clear();
	Pointer.bPressed = Pointer.bDown = true;
	Router.Process(*Root, Pointer, {}, Events);
	E_EXPECT_TRUE(Button->State.bPressed && Button->State.bFocused);
	Pointer.bPressed = Pointer.bDown = false;
	Pointer.bReleased                = true;
	Events.clear();
	Router.Process(*Root, Pointer, {}, Events);
	E_EXPECT_TRUE(Events.size() == 2 && Events[0].Type == EUIEventType::Released && Events[1].Type == EUIEventType::Clicked);
	E_EXPECT_FALSE(Button->State.bPressed);

	// 누른 뒤 밖에서 떼면 클릭 아님
	Pointer.bReleased = false;
	Pointer.bPressed = Pointer.bDown = true;
	Router.Process(*Root, Pointer, {}, Events);
	Pointer.bPressed = Pointer.bDown = false;
	Pointer.bReleased                = true;
	Pointer.Position                 = FVector2(300.0f, 300.0f);
	Events.clear();
	E_EXPECT_TRUE(Router.Process(*Root, Pointer, {}, Events)); // 누른 채 끌어 나간 떼기까지는 UI가 가져간다
	bool bClicked = false;
	for (const FUIEvent& Event : Events)
	{
		bClicked = bClicked || Event.Type == EUIEventType::Clicked;
	}
	E_EXPECT_FALSE(bClicked);
	E_EXPECT_FALSE(Button->State.bHovered);
	Pointer.bReleased = false;
	Events.clear();
	E_EXPECT_FALSE(Router.Process(*Root, Pointer, {}, Events)); // 다음 프레임부터는 게임으로

	// 비활성: 입력은 막지만(true) 이벤트 없음
	Button->bEnabled  = false;
	Pointer.Position  = FVector2(50.0f, 20.0f);
	Pointer.bReleased = false;
	Pointer.bPressed = Pointer.bDown = true;
	Events.clear();
	E_EXPECT_TRUE(Router.Process(*Root, Pointer, {}, Events));
	E_EXPECT_TRUE(Events.empty());
}

E_TEST(UIInput_KeyboardFocusAndWheel)
{
	auto       Root   = FUIWidget::Create(EUIWidgetType::VerticalBox);
	FUIWidget* First  = AddChild(*Root, EUIWidgetType::Button, "First");
	FUIWidget* Second = AddChild(*Root, EUIWidgetType::Button, "Second");
	First->MinSize = Second->MinSize = FVector2(50.0f, 20.0f);
	Layout(*Root, 100.0f, 100.0f);

	FUIInputRouter        Router;
	std::vector<FUIEvent> Events;
	FUIKeyInput           Keys;
	Keys.bFocusNext = true;
	Router.Process(*Root, {}, Keys, Events);
	E_EXPECT_TRUE(First->State.bFocused);
	Router.Process(*Root, {}, Keys, Events);
	E_EXPECT_TRUE(Second->State.bFocused && !First->State.bFocused);
	Router.Process(*Root, {}, Keys, Events); // 끝에서 처음으로
	E_EXPECT_TRUE(First->State.bFocused);
	Keys            = {};
	Keys.bActivate  = true;
	Events.clear();
	Router.Process(*Root, {}, Keys, Events);
	E_EXPECT_TRUE(Events.size() == 1 && Events[0].Type == EUIEventType::Clicked && Events[0].WidgetName == "First");

	// 휠: 스크롤 박스 안 위젯 위에서
	auto       Canvas = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Scroll = AddChild(*Canvas, EUIWidgetType::ScrollBox, "Scroll");
	Scroll->Slot.Offsets = FUIMargin(0.0f, 0.0f, 100.0f, 100.0f);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		AddImage(*Scroll, "Row", 50.0f, 50.0f);
	}
	Layout(*Canvas, 400.0f, 400.0f);
	FUIPointerInput Pointer;
	Pointer.bInside  = true;
	Pointer.Position = FVector2(20.0f, 20.0f);
	Pointer.Wheel    = -1.0f; // 아래로
	Router.Process(*Canvas, Pointer, {}, Events);
	E_EXPECT_NEAR(Scroll->State.ScrollOffset, FUIInputRouter::WheelStep, 0.01f);
	Pointer.Wheel = -10.0f;
	Router.Process(*Canvas, Pointer, {}, Events);
	E_EXPECT_NEAR(Scroll->State.ScrollOffset, 100.0f, 0.01f);
}

// ---------------------------------------------------------------- 글꼴 / 그리기

E_TEST(UIFont_DecodeUtf8)
{
	const std::vector<uint32> Codepoints = DecodeUtf8("A가\xF0\x9F\x98\x80\xFF");
	E_EXPECT_EQ(Codepoints.size(), size_t(4));
	if (Codepoints.size() == 4)
	{
		E_EXPECT_EQ(Codepoints[0], 0x41u);
		E_EXPECT_EQ(Codepoints[1], 0xAC00u);
		E_EXPECT_EQ(Codepoints[2], 0x1F600u);
		E_EXPECT_EQ(Codepoints[3], 0xFFFDu);
	}
	E_EXPECT_EQ(DecodeUtf8("\xE1\x84").size(), size_t(1)); // 잘린 문자
}

E_TEST(UIFont_SdfLayoutAndWrap)
{
	FUIFont* Font = FUIFontLibrary::Get().GetDefaultFont();
	if (Font == nullptr)
	{
		E_LOG(LogUI, Warning, "시스템 글꼴이 없어 글꼴 테스트를 건너뜁니다");
		return;
	}
	const uint32  VersionBefore = Font->GetAtlasVersion();
	FUITextLayout Layout;
	Font->Layout("안녕 UI", 32.0f, 0.0f, Layout);
	E_EXPECT_EQ(Layout.Lines.size(), size_t(1));
	E_EXPECT_EQ(Layout.Glyphs.size(), size_t(4)); // 공백은 모양 없음
	E_EXPECT_TRUE(Layout.Size.X > 32.0f && Layout.Size.X < 32.0f * 5.0f);
	E_EXPECT_TRUE(Font->GetAtlasVersion() > VersionBefore);
	// 아틀라스에 거리 값이 들어갔다 (가장자리 = 128 근처, 안쪽 > 128)
	uint8 MaxValue = 0;
	for (const uint8 Value : Font->GetAtlasPixels())
	{
		MaxValue = FMath::Max(MaxValue, Value);
	}
	E_EXPECT_TRUE(MaxValue > 128);

	// 같은 글자는 다시 굽지 않는다
	const uint32 VersionAfter = Font->GetAtlasVersion();
	Font->Layout("녕안", 16.0f, 0.0f, Layout);
	E_EXPECT_EQ(Font->GetAtlasVersion(), VersionAfter);

	// 줄바꿈: 단어 두 개가 한 줄에 안 들어가면 공백에서 끊는다
	const float WordWidth = Font->Measure("안녕하세요", 32.0f, 0.0f).X;
	Font->Layout("안녕하세요 안녕하세요", 32.0f, WordWidth * 1.5f, Layout);
	E_EXPECT_EQ(Layout.Lines.size(), size_t(2));
	E_EXPECT_NEAR(Layout.Lines[0].Width, WordWidth, 0.5f);
	// 한 단어가 너무 길면 글자 단위
	Font->Layout("안녕하세요", 32.0f, WordWidth * 0.5f, Layout);
	E_EXPECT_TRUE(Layout.Lines.size() >= 2);
	E_EXPECT_TRUE(Layout.Size.X <= WordWidth * 0.5f + 0.5f);
	// 명시적 줄바꿈
	Font->Layout("A\nB\n", 20.0f, 0.0f, Layout);
	E_EXPECT_EQ(Layout.Lines.size(), size_t(3));
	E_EXPECT_NEAR(Layout.Size.Y, Font->GetLineHeight(20.0f) * 3.0f, 0.01f);
}

E_TEST(UIPainter_ButtonStateBrushAndBatches)
{
	FUIAsset   Asset;
	FUIWidget* Button = AddChild(*Asset.Root, EUIWidgetType::Button, "Ok");
	Button->Slot.Offsets  = FUIMargin(10.0f, 20.0f, 100.0f, 40.0f);
	Button->Brush.Color   = FVector4(1.0f, 0.0f, 0.0f, 1.0f);
	Button->HoveredBrush.Color = FVector4(0.0f, 1.0f, 0.0f, 1.0f);
	FUIWidget* Image = AddImage(*Asset.Root, "Icon", 16.0f, 16.0f);
	Image->Slot.bAutoSize = true;
	Image->Brush.Texture  = "Icon.png";

	FUIInstance           Instance(Asset);
	FFakeMeasurer         Measurer;
	std::vector<FUIEvent> Events;
	// 설계 1920x1080 → 화면 960x540 (배율 0.5), 화면 (100, 50)에 놓음
	const FUIRect Viewport(FVector2(100.0f, 50.0f), FVector2(1060.0f, 590.0f));
	FUIPointerInput Pointer;
	Pointer.bInside  = true;
	Pointer.Position = FVector2(100.0f + 30.0f, 50.0f + 20.0f); // UI (60, 40) → 버튼 위
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	E_EXPECT_TRUE(Instance.Update(Viewport, &Pointer, nullptr, Fonts, Events));
	E_EXPECT_NEAR(Instance.GetTransform().Scale, 0.5f, 1e-5f);

	FUIDrawList List;
	Instance.Paint(List, Fonts);
	E_EXPECT_EQ(List.Quads.size(), size_t(2));
	E_EXPECT_EQ(List.Batches.size(), size_t(2)); // 흰색 / 파일 텍스처
	if (List.Quads.size() == 2)
	{
		// 버튼: 호버 브러시(초록), 화면 픽셀 = UI × 0.5 + (100, 50)
		E_EXPECT_NEAR(List.Quads[0].Color.Y, 1.0f, 1e-4f);
		E_EXPECT_NEAR(List.Quads[0].Color.X, 0.0f, 1e-4f);
		E_EXPECT_NEAR(List.Quads[0].Rect.X, 105.0f, 1e-3f);
		E_EXPECT_NEAR(List.Quads[0].Rect.Y, 60.0f, 1e-3f);
		E_EXPECT_NEAR(List.Quads[0].Rect.Z, 155.0f, 1e-3f);
		E_EXPECT_NEAR(List.Quads[0].Params.X, 3.0f, 1e-3f); // 모서리 6 × 0.5
		E_EXPECT_EQ(List.Batches[1].Texture.Path, std::string("Icon.png"));
	}
	(void)Measurer;
}

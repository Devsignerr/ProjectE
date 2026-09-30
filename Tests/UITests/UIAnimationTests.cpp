#include "Core/Testing/TestFramework.h"
#include "UI/UIAnimation.h"
#include "UI/UIAsset.h"
#include "UI/UIFont.h"
#include "UI/UIInput.h"
#include "UI/UIInstance.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"

namespace
{
	FUIAnimTrack MakeTrack(const char* Widget, EUIAnimProperty Property, std::initializer_list<FUIAnimKey> Keys)
	{
		FUIAnimTrack Track;
		Track.Widget   = Widget;
		Track.Property = Property;
		Track.Keys     = Keys;
		Track.SortKeys();
		return Track;
	}
} // namespace

E_TEST(UIAnimation_TrackEvaluateAndEase)
{
	const FUIAnimTrack Track = MakeTrack("A", EUIAnimProperty::Opacity,
	                                     { { 1.0f, 10.0f, EUIAnimInterp::Linear }, { 0.0f, 0.0f, EUIAnimInterp::Linear }, { 2.0f, 10.0f, EUIAnimInterp::Constant } });
	float Value = 0.0f;
	E_EXPECT_TRUE(Track.Evaluate(-1.0f, Value) && Value == 0.0f); // 첫 키 전
	E_EXPECT_TRUE(Track.Evaluate(0.5f, Value));
	E_EXPECT_NEAR(Value, 5.0f, 1e-4f);
	E_EXPECT_TRUE(Track.Evaluate(1.5f, Value));
	E_EXPECT_NEAR(Value, 10.0f, 1e-4f);
	E_EXPECT_TRUE(Track.Evaluate(5.0f, Value) && Value == 10.0f); // 마지막 키 뒤

	FUIAnimTrack Empty;
	E_EXPECT_FALSE(Empty.Evaluate(0.0f, Value));

	// 이징: 양 끝 고정, 가운데 대칭
	for (int32 Interp = 0; Interp < static_cast<int32>(EUIAnimInterp::Count); ++Interp)
	{
		const EUIAnimInterp Mode = static_cast<EUIAnimInterp>(Interp);
		E_EXPECT_NEAR(FUIAnimMath::Ease(Mode, 1.0f), 1.0f, 1e-5f);
		E_EXPECT_NEAR(FUIAnimMath::Ease(Mode, 0.0f), 0.0f, 1e-5f);
	}
	E_EXPECT_NEAR(FUIAnimMath::Ease(EUIAnimInterp::EaseInOut, 0.5f), 0.5f, 1e-5f);
	E_EXPECT_TRUE(FUIAnimMath::Ease(EUIAnimInterp::EaseIn, 0.5f) < 0.5f && FUIAnimMath::Ease(EUIAnimInterp::EaseOut, 0.5f) > 0.5f);

	// SetKey: 같은 시간이면 값만, 아니면 정렬 삽입
	FUIAnimTrack Edit;
	Edit.SetKey(1.0f, 1.0f);
	Edit.SetKey(0.0f, 0.0f);
	E_EXPECT_EQ(Edit.SetKey(1.0005f, 7.0f), 1);
	E_EXPECT_EQ(Edit.Keys.size(), size_t(2));
	E_EXPECT_NEAR(Edit.Keys[1].Value, 7.0f, 0.0f);
}

E_TEST(UIAnimation_RenderTransformAffectsVisualOnly)
{
	auto       Root  = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Panel = Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
	Panel->Name         = "Panel";
	Panel->Slot.Offsets = FUIMargin(100.0f, 100.0f, 200.0f, 100.0f);
	Panel->ContentPadding = FUIMargin(0.0f);
	FUIWidget* Inner    = Panel->AddChild(FUIWidget::Create(EUIWidgetType::Image));
	Inner->Name         = "Inner";
	Inner->ImageSize    = FVector2(20.0f, 20.0f);
	Inner->Slot.HAlign  = EUIHAlign::Left;
	Inner->Slot.VAlign  = EUIVAlign::Top;
	Panel->RenderScale       = FVector2(2.0f, 2.0f); // 가운데(200, 150) 기준 2배
	Panel->RenderTranslation = FVector2(10.0f, 0.0f);
	Root->AssignIds();
	FUILayout::Compute(*Root, FVector2(800.0f, 600.0f), FUIFontLibrary::Get());

	// 레이아웃은 그대로, 보이는 영역만 변환
	E_EXPECT_NEAR(Panel->State.Geometry.Min.X, 100.0f, 1e-4f);
	E_EXPECT_NEAR(Panel->State.VisualGeometry.Min.X, 10.0f, 1e-4f);  // 200 - 100×2 + 10
	E_EXPECT_NEAR(Panel->State.VisualGeometry.Max.X, 410.0f, 1e-4f); // 200 + 100×2 + 10
	E_EXPECT_NEAR(Panel->State.VisualGeometry.Min.Y, 50.0f, 1e-4f);
	// 자식에게 누적: 레이아웃 (100,100)~(120,120) → (10,50)~(50,90)
	E_EXPECT_NEAR(Inner->State.VisualGeometry.Min.X, 10.0f, 1e-4f);
	E_EXPECT_NEAR(Inner->State.VisualGeometry.Max.X, 50.0f, 1e-4f);
	E_EXPECT_NEAR(Inner->State.VisualGeometry.Max.Y, 90.0f, 1e-4f);
	// 맞히기도 보이는 영역 기준
	E_EXPECT_TRUE(FUIInputRouter::HitTest(*Root, FVector2(30.0f, 70.0f)) == Inner);
	E_EXPECT_TRUE(FUIInputRouter::HitTest(*Root, FVector2(390.0f, 230.0f)) == Panel);

	// 그리기: 패널 배경 사각형이 보이는 영역에, 모서리 반지름은 배율만큼
	FUIDrawList  List;
	FUITransform Transform;
	FUIPainter::Paint(*Root, Transform, FUIRect::Infinite(), FUIFontLibrary::Get(), List);
	E_EXPECT_TRUE(List.Quads.size() >= 2);
	if (!List.Quads.empty())
	{
		E_EXPECT_NEAR(List.Quads[0].Rect.X, 10.0f, 1e-3f);
		E_EXPECT_NEAR(List.Quads[0].Rect.Z, 410.0f, 1e-3f);
		E_EXPECT_NEAR(List.Quads[0].Params.X, Panel->Brush.CornerRadius * 2.0f, 1e-3f);
	}
}

E_TEST(UIAnimation_PlaybackLoopsAndFinishEvent)
{
	FUIAsset   Asset;
	FUIWidget* Menu = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
	Menu->Name      = "Menu";
	FUIAnimation Intro;
	Intro.Name   = "Intro";
	Intro.Length = 1.0f;
	Intro.Tracks.push_back(MakeTrack("Menu", EUIAnimProperty::Opacity, { { 0.0f, 0.0f, EUIAnimInterp::Linear }, { 1.0f, 1.0f, EUIAnimInterp::Linear } }));
	Intro.Tracks.push_back(MakeTrack("Menu", EUIAnimProperty::TranslationY, { { 0.0f, 40.0f, EUIAnimInterp::EaseOut }, { 1.0f, 0.0f, EUIAnimInterp::Linear } }));
	Intro.Tracks.push_back(MakeTrack("Gone", EUIAnimProperty::ScaleX, { { 0.0f, 2.0f, EUIAnimInterp::Linear } })); // 없는 위젯은 무시
	Asset.Animations.push_back(Intro);

	// JSON 왕복 (버전 2)
	FUIAsset Loaded;
	E_EXPECT_TRUE(Loaded.FromJsonString(Asset.ToJsonString()));
	E_EXPECT_TRUE(Loaded.Animations.size() == 1 && Loaded.Animations[0] == Intro);

	FUIInstance           Instance(Loaded);
	std::vector<FUIEvent> Events;
	E_EXPECT_FALSE(Instance.PlayAnimation("Nope"));
	E_EXPECT_TRUE(Instance.PlayAnimation("Intro"));
	FUIWidget* Played = Instance.FindWidget("Menu");
	E_EXPECT_NEAR(Played->RenderOpacity, 0.0f, 1e-5f); // 재생 즉시 시작 값
	Instance.TickAnimations(0.5f, Events);
	E_EXPECT_NEAR(Played->RenderOpacity, 0.5f, 1e-4f);
	E_EXPECT_TRUE(Played->RenderTranslation.Y < 20.0f); // EaseOut: 절반 시간에 절반보다 더 왔다
	E_EXPECT_TRUE(Instance.IsAnimationPlaying("Intro") && Events.empty());
	Instance.TickAnimations(0.6f, Events);
	E_EXPECT_NEAR(Played->RenderOpacity, 1.0f, 1e-5f); // 끝 값에 멈춤
	E_EXPECT_FALSE(Instance.IsAnimationPlaying("Intro"));
	E_EXPECT_TRUE(Events.size() == 1 && Events[0].Type == EUIEventType::AnimationFinished && Events[0].WidgetName == "Intro");

	// 무한 반복 + 거꾸로
	Events.clear();
	Instance.PlayAnimation("Intro", 0, -1.0f);
	E_EXPECT_NEAR(Played->RenderOpacity, 1.0f, 1e-5f);
	Instance.TickAnimations(2.25f, Events);
	E_EXPECT_TRUE(Instance.IsAnimationPlaying("Intro") && Events.empty());
	E_EXPECT_NEAR(Played->RenderOpacity, 0.75f, 1e-3f);
	Instance.StopAnimation("Intro");
	E_EXPECT_FALSE(Instance.IsAnimationPlaying("Intro"));

	// 2번 반복: 2초 뒤 끝
	Instance.PlayAnimation("Intro", 2);
	Instance.TickAnimations(1.5f, Events);
	E_EXPECT_TRUE(Instance.IsAnimationPlaying("Intro"));
	Instance.TickAnimations(0.6f, Events);
	E_EXPECT_FALSE(Instance.IsAnimationPlaying("Intro"));
}

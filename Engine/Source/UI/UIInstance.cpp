#include "UI/UIInstance.h"

#include "UI/UIFont.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"

FUIInstance::FUIInstance()
{
	Asset.Root->AssignIds();
}

FUIInstance::FUIInstance(const FUIAsset& InAsset)
{
	SetAsset(InAsset);
}

void FUIInstance::SetAsset(const FUIAsset& InAsset)
{
	Asset        = InAsset.Clone();
	Router       = FUIInputRouter{};
	bPointerOver = false;
	// 에셋에 들어 있는 글자는 지금 굽는다 (실행 중 바뀌는 글자만 처음 쓸 때 굽힌다)
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	Asset.Root->ForEach([&Fonts](FUIWidget& Widget) {
		if (Widget.Type == EUIWidgetType::Text && !Widget.Text.empty())
		{
			if (FUIFont* Font = Fonts.GetFont(Widget.Font))
			{
				Font->Prebake(Widget.Text);
			}
		}
	});
}

void FUIInstance::Layout(const FUIRect& InViewport, FUIFontLibrary& Fonts)
{
	Viewport         = InViewport;
	Transform.Scale  = FMath::Max(FUILayout::ComputeScale(Asset.ScaleMode, Asset.DesignSize, InViewport.GetSize()), 0.01f);
	Transform.Offset = InViewport.Min;
	FUILayout::Compute(*Asset.Root, InViewport.GetSize() / Transform.Scale, Fonts);
}

bool FUIInstance::Update(const FUIRect& InViewport, const FUIPointerInput* PointerPixels, const FUIKeyInput* Keys, FUIFontLibrary& Fonts,
                         std::vector<FUIEvent>& OutEvents, float DeltaSeconds)
{
	Layout(InViewport, Fonts);

	FUIPointerInput Pointer;
	if (PointerPixels != nullptr)
	{
		Pointer          = *PointerPixels;
		Pointer.bInside  = Pointer.bInside && InViewport.Contains(Pointer.Position);
		Pointer.Position = Transform.ToUi(Pointer.Position);
	}
	const FUIKeyInput NoKeys;
	const float       ScrollBefore = Pointer.Wheel;
	bPointerOver                   = Router.Process(*Asset.Root, Pointer, Keys != nullptr ? *Keys : NoKeys, OutEvents);
	if (ScrollBefore != 0.0f)
	{
		// 스크롤 위치가 바뀌었으면 같은 프레임에 반영
		FUILayout::Compute(*Asset.Root, InViewport.GetSize() / Transform.Scale, Fonts);
	}

	// 포커스된 텍스트 상자: 캐럿 깜빡임 + 캐럿이 보이도록 가로 스크롤
	if (FUIWidget* Focused = Router.GetFocusedId() != 0 ? Asset.Root->FindById(Router.GetFocusedId()) : nullptr;
	    Focused != nullptr && Focused->Type == EUIWidgetType::TextBox)
	{
		Focused->State.CaretTime += DeltaSeconds;
		if (FUIFont* Font = Fonts.GetFont(Focused->Font))
		{
			std::vector<float> Positions;
			Font->GetCaretPositions(Focused->Text, Focused->FontSize, Positions);
			const int32 Caret        = FMath::Clamp(Focused->State.CaretIndex, 0, static_cast<int32>(Positions.size()) - 1);
			const float CaretX       = Positions[static_cast<size_t>(Caret)];
			const float ContentWidth = FMath::Max(Focused->State.Geometry.Inset(Focused->ContentPadding).GetWidth() - 2.0f, 1.0f);
			float&      Scroll       = Focused->State.TextScroll;
			Scroll                   = FMath::Clamp(Scroll, CaretX - ContentWidth, CaretX);
			Scroll                   = FMath::Clamp(Scroll, 0.0f, FMath::Max(Positions.back() - ContentWidth, 0.0f));
		}
	}
	return bPointerOver;
}

void FUIInstance::Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const
{
	FUIPainter::Paint(*Asset.Root, Transform, Viewport, Fonts, Out);
}

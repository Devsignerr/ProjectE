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
}

void FUIInstance::Layout(const FUIRect& InViewport, FUIFontLibrary& Fonts)
{
	Viewport         = InViewport;
	Transform.Scale  = FMath::Max(FUILayout::ComputeScale(Asset.ScaleMode, Asset.DesignSize, InViewport.GetSize()), 0.01f);
	Transform.Offset = InViewport.Min;
	FUILayout::Compute(*Asset.Root, InViewport.GetSize() / Transform.Scale, Fonts);
}

bool FUIInstance::Update(const FUIRect& InViewport, const FUIPointerInput* PointerPixels, const FUIKeyInput* Keys, FUIFontLibrary& Fonts,
                         std::vector<FUIEvent>& OutEvents)
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
	return bPointerOver;
}

void FUIInstance::Paint(FUIDrawList& Out, FUIFontLibrary& Fonts) const
{
	FUIPainter::Paint(*Asset.Root, Transform, Viewport, Fonts, Out);
}

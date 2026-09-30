#include "UI/UIPainter.h"

#include "UI/Widget.h"

#include <algorithm>

void FUIDrawList::AddQuad(const FUIDrawQuad& Quad, const FUITextureRef& Texture, const FUIRect& Clip)
{
	if (Batches.empty() || !(Batches.back().Texture == Texture) || !(Batches.back().Clip == Clip))
	{
		FUIDrawBatch Batch;
		Batch.Texture   = Texture;
		Batch.Clip      = Clip;
		Batch.FirstQuad = static_cast<uint32>(Quads.size());
		Batches.push_back(std::move(Batch));
	}
	Quads.push_back(Quad);
	++Batches.back().QuadCount;
}

namespace
{
	FVector4 ToLinear(const FVector4& Srgb, float Opacity)
	{
		FVector4 Linear = UISrgbToLinear(Srgb);
		Linear.W *= Opacity;
		return Linear;
	}

	const FUIBrush& SelectButtonBrush(const FUIWidget& Widget, bool bEnabled)
	{
		if (!bEnabled)
		{
			return Widget.DisabledBrush;
		}
		if (Widget.State.bPressed && Widget.State.bHovered)
		{
			return Widget.PressedBrush;
		}
		if (Widget.State.bHovered || Widget.State.bFocused)
		{
			return Widget.HoveredBrush;
		}
		return Widget.Brush;
	}

	void PaintProgressBar(const FUIWidget& Widget, float Opacity, const FUITransform& Transform, const FUIRect& Clip, FUIDrawList& Out)
	{
		const FUIRect& Rect = Widget.State.Geometry;
		FUIPainter::PaintBrush(Widget.Brush, Rect, Opacity, Transform, Clip, Out);
		const float Percent = FMath::Clamp(Widget.Percent, 0.0f, 1.0f);
		if (Percent <= 0.0f)
		{
			return;
		}
		FUIRect Fill = Rect;
		switch (Widget.FillDirection)
		{
		case EUIFillDirection::LeftToRight: Fill.Max.X = Rect.Min.X + Rect.GetWidth() * Percent; break;
		case EUIFillDirection::RightToLeft: Fill.Min.X = Rect.Max.X - Rect.GetWidth() * Percent; break;
		case EUIFillDirection::BottomToTop: Fill.Min.Y = Rect.Max.Y - Rect.GetHeight() * Percent; break;
		case EUIFillDirection::TopToBottom: Fill.Max.Y = Rect.Min.Y + Rect.GetHeight() * Percent; break;
		}
		FUIPainter::PaintBrush(Widget.FillBrush, Fill, Opacity, Transform, Clip, Out);
	}

	void PaintScrollbar(const FUIWidget& Widget, float Opacity, const FUITransform& Transform, const FUIRect& Clip, FUIDrawList& Out)
	{
		if (Widget.State.ScrollMax <= 0.0f || Widget.ScrollbarWidth <= 0.0f)
		{
			return;
		}
		const FUIRect& Rect        = Widget.State.Geometry;
		const bool     bHorizontal = Widget.Orientation == EUIOrientation::Horizontal;
		const float    ViewLen     = bHorizontal ? Rect.GetWidth() : Rect.GetHeight();
		const float    ContentLen  = ViewLen + Widget.State.ScrollMax;
		const float    ThumbLen    = FMath::Max(ViewLen * ViewLen / ContentLen, FMath::Min(16.0f, ViewLen));
		const float    ThumbPos    = (ViewLen - ThumbLen) * Widget.State.ScrollOffset / Widget.State.ScrollMax;

		FUIBrush Thumb;
		Thumb.Color        = Widget.ScrollbarColor;
		Thumb.CornerRadius = Widget.ScrollbarWidth * 0.5f;
		FUIRect ThumbRect;
		if (bHorizontal)
		{
			ThumbRect = FUIRect(FVector2(Rect.Min.X + ThumbPos, Rect.Max.Y - Widget.ScrollbarWidth), FVector2(Rect.Min.X + ThumbPos + ThumbLen, Rect.Max.Y));
		}
		else
		{
			ThumbRect = FUIRect(FVector2(Rect.Max.X - Widget.ScrollbarWidth, Rect.Min.Y + ThumbPos), FVector2(Rect.Max.X, Rect.Min.Y + ThumbPos + ThumbLen));
		}
		FUIPainter::PaintBrush(Thumb, ThumbRect, Opacity, Transform, Clip, Out);
	}

	void PaintWidget(const FUIWidget& Widget, float ParentOpacity, bool bParentEnabled, const FUITransform& Transform, const FUIRect& ViewportClip,
	                 FUIFontLibrary& Fonts, FUIDrawList& Out)
	{
		if (Widget.Visibility == EUIVisibility::Collapsed || Widget.Visibility == EUIVisibility::Hidden)
		{
			return;
		}
		const float Opacity  = ParentOpacity * FMath::Clamp(Widget.RenderOpacity, 0.0f, 1.0f);
		const bool  bEnabled = bParentEnabled && Widget.bEnabled;
		if (Opacity <= 0.0f)
		{
			return;
		}
		const FUIRect Clip = ViewportClip.Intersect(Transform.ToPixels(Widget.State.Clip));
		if (Clip.IsEmpty())
		{
			return;
		}

		switch (Widget.Type)
		{
		case EUIWidgetType::Border:
		case EUIWidgetType::Image:
			FUIPainter::PaintBrush(Widget.Brush, Widget.State.Geometry, Opacity, Transform, Clip, Out);
			break;
		case EUIWidgetType::Button:
			FUIPainter::PaintBrush(SelectButtonBrush(Widget, bEnabled), Widget.State.Geometry, Opacity, Transform, Clip, Out);
			break;
		case EUIWidgetType::Text:
			FUIPainter::PaintText(Widget, Widget.State.Geometry, Opacity, Transform, Clip, Fonts, Out);
			break;
		case EUIWidgetType::ProgressBar:
			PaintProgressBar(Widget, Opacity, Transform, Clip, Out);
			break;
		default:
			break;
		}

		if (Widget.Type == EUIWidgetType::Canvas)
		{
			// ZOrder 오름차순 (같으면 원래 순서)
			std::vector<const FUIWidget*> Sorted;
			Sorted.reserve(Widget.Children.size());
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				Sorted.push_back(Child.get());
			}
			std::stable_sort(Sorted.begin(), Sorted.end(), [](const FUIWidget* A, const FUIWidget* B) { return A->Slot.ZOrder < B->Slot.ZOrder; });
			for (const FUIWidget* Child : Sorted)
			{
				PaintWidget(*Child, Opacity, bEnabled, Transform, ViewportClip, Fonts, Out);
			}
		}
		else
		{
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				PaintWidget(*Child, Opacity, bEnabled, Transform, ViewportClip, Fonts, Out);
			}
		}

		if (Widget.Type == EUIWidgetType::ScrollBox)
		{
			PaintScrollbar(Widget, Opacity, Transform, Clip, Out);
		}
	}
} // namespace

void FUIPainter::Paint(const FUIWidget& Root, const FUITransform& Transform, const FUIRect& ViewportClip, FUIFontLibrary& Fonts, FUIDrawList& Out)
{
	PaintWidget(Root, 1.0f, true, Transform, ViewportClip, Fonts, Out);
}

void FUIPainter::PaintBrush(const FUIBrush& Brush, const FUIRect& Rect, float Opacity, const FUITransform& Transform, const FUIRect& ClipPixels,
                            FUIDrawList& Out)
{
	if (!Brush.IsVisible() || Rect.IsEmpty())
	{
		return;
	}
	if (Brush.DrawAs == EUIBrushDrawAs::NineSlice && !Brush.Texture.empty())
	{
		// 가장자리 두께(UI 단위) = 텍스처 비율 × 원본 크기. 사각형보다 두꺼우면 비율대로 줄인다
		const FUIMargin M      = Brush.Margin;
		float           Left   = FMath::Max(M.Left, 0.0f) * Brush.TextureSize.X;
		float           Right  = FMath::Max(M.Right, 0.0f) * Brush.TextureSize.X;
		float           Top    = FMath::Max(M.Top, 0.0f) * Brush.TextureSize.Y;
		float           Bottom = FMath::Max(M.Bottom, 0.0f) * Brush.TextureSize.Y;
		if (Left + Right > Rect.GetWidth() && Left + Right > 0.0f)
		{
			const float Shrink = Rect.GetWidth() / (Left + Right);
			Left *= Shrink;
			Right *= Shrink;
		}
		if (Top + Bottom > Rect.GetHeight() && Top + Bottom > 0.0f)
		{
			const float Shrink = Rect.GetHeight() / (Top + Bottom);
			Top *= Shrink;
			Bottom *= Shrink;
		}
		const float Xs[4] = { Rect.Min.X, Rect.Min.X + Left, Rect.Max.X - Right, Rect.Max.X };
		const float Ys[4] = { Rect.Min.Y, Rect.Min.Y + Top, Rect.Max.Y - Bottom, Rect.Max.Y };
		const float Us[4] = { 0.0f, M.Left, 1.0f - M.Right, 1.0f };
		const float Vs[4] = { 0.0f, M.Top, 1.0f - M.Bottom, 1.0f };
		FUITextureRef Texture;
		Texture.Path = Brush.Texture;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				if (Xs[Col + 1] <= Xs[Col] || Ys[Row + 1] <= Ys[Row])
				{
					continue;
				}
				const FVector2 Min = Transform.ToPixels(FVector2(Xs[Col], Ys[Row]));
				const FVector2 Max = Transform.ToPixels(FVector2(Xs[Col + 1], Ys[Row + 1]));
				FUIDrawQuad    Quad;
				Quad.Rect           = FVector4(Min.X, Min.Y, Max.X, Max.Y);
				Quad.UV             = FVector4(Us[Col], Vs[Row], Us[Col + 1], Vs[Row + 1]);
				Quad.Color          = ToLinear(Brush.Color, Opacity);
				Quad.SecondaryColor = FVector4();
				Quad.Params         = FVector4(0.0f, 0.0f, static_cast<float>(EUIDrawMode::Box), 0.0f);
				Out.AddQuad(Quad, Texture, ClipPixels);
			}
		}
		return;
	}

	const FUIRect Pixels    = Transform.ToPixels(Rect);
	const float   HalfMin   = FMath::Min(Pixels.GetWidth(), Pixels.GetHeight()) * 0.5f;
	const float   Radius    = FMath::Clamp(Brush.CornerRadius * Transform.Scale, 0.0f, HalfMin);
	const float   Border    = FMath::Clamp(Brush.BorderWidth * Transform.Scale, 0.0f, HalfMin);

	FUIDrawQuad Quad;
	Quad.Rect           = FVector4(Pixels.Min.X, Pixels.Min.Y, Pixels.Max.X, Pixels.Max.Y);
	Quad.UV             = FVector4(0.0f, 0.0f, 1.0f, 1.0f);
	Quad.Color          = ToLinear(Brush.Color, Opacity);
	Quad.SecondaryColor = ToLinear(Brush.BorderColor, Opacity);
	Quad.Params         = FVector4(Radius, Border, static_cast<float>(EUIDrawMode::Box), 0.0f);

	FUITextureRef Texture;
	Texture.Path = Brush.Texture;
	Out.AddQuad(Quad, Texture, ClipPixels);
}

void FUIPainter::PaintText(const FUIWidgetData& TextWidget, const FUIRect& Geometry, float Opacity, const FUITransform& Transform,
                           const FUIRect& ClipPixels, FUIFontLibrary& Fonts, FUIDrawList& Out)
{
	if (TextWidget.Text.empty() || TextWidget.FontSize <= 0.0f)
	{
		return;
	}
	FUIFont* Font = Fonts.GetFont(TextWidget.Font);
	if (Font == nullptr)
	{
		return;
	}
	FUITextLayout Layout;
	Font->Layout(TextWidget.Text, TextWidget.FontSize, TextWidget.bWrap ? Geometry.GetWidth() : 0.0f, Layout);
	if (Layout.Glyphs.empty())
	{
		return;
	}

	const float   Range     = FUIFont::GetDistanceRange(TextWidget.FontSize) * Transform.Scale;
	const float   Outline   = FMath::Max(TextWidget.OutlineWidth, 0.0f) * Transform.Scale;
	FUITextureRef Texture;
	Texture.Font = Font;

	const auto Emit = [&](const FVector2& Offset, const FVector4& Color, const FVector4& OutlineColor) {
		for (const FUIPlacedGlyph& Glyph : Layout.Glyphs)
		{
			const FUITextLine& Line   = Layout.Lines[Glyph.Line];
			float              ShiftX = 0.0f;
			if (TextWidget.Justify == EUITextJustify::Center)
			{
				ShiftX = (Geometry.GetWidth() - Line.Width) * 0.5f;
			}
			else if (TextWidget.Justify == EUITextJustify::Right)
			{
				ShiftX = Geometry.GetWidth() - Line.Width;
			}
			const FVector2 Min = Transform.ToPixels(Geometry.Min + Offset + Glyph.Position + FVector2(ShiftX, 0.0f));
			const FVector2 Max = Min + Glyph.Size * Transform.Scale;

			FUIDrawQuad Quad;
			Quad.Rect           = FVector4(Min.X, Min.Y, Max.X, Max.Y);
			// 텍셀 좌표 (같은 프레임에 아틀라스가 커져도 유효 — 셰이더가 텍스처 크기로 나눈다)
			Quad.UV             = FVector4(static_cast<float>(Glyph.AtlasX), static_cast<float>(Glyph.AtlasY),
			                               static_cast<float>(Glyph.AtlasX + Glyph.AtlasWidth), static_cast<float>(Glyph.AtlasY + Glyph.AtlasHeight));
			Quad.Color          = Color;
			Quad.SecondaryColor = OutlineColor;
			Quad.Params         = FVector4(Range, Outline, static_cast<float>(EUIDrawMode::SdfText), 0.0f);
			Out.AddQuad(Quad, Texture, ClipPixels);
		}
	};

	if (TextWidget.ShadowOffset != FVector2::ZeroVector && TextWidget.ShadowColor.W > 0.0f)
	{
		const FVector4 Shadow = ToLinear(TextWidget.ShadowColor, Opacity);
		Emit(TextWidget.ShadowOffset, Shadow, Shadow);
	}
	Emit(FVector2::ZeroVector, ToLinear(TextWidget.TextColor, Opacity), ToLinear(TextWidget.OutlineColor, Opacity));
}

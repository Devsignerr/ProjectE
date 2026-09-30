#include "UI/UILayout.h"

#include "UI/Widget.h"

namespace
{
	bool IsCollapsed(const FUIWidget& Widget) { return Widget.Visibility == EUIVisibility::Collapsed; }

	FVector2 Max2(const FVector2& A, const FVector2& B) { return { FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y) }; }

	// 슬롯 여백을 포함한 자식 크기
	FVector2 SlotDesired(const FUIWidget& Child) { return Child.State.DesiredSize + Child.Slot.Padding.GetTotal(); }

	FVector2 ComputeDesired(FUIWidget& Widget, IUITextMeasurer& Measurer)
	{
		FVector2 Desired;
		if (IsCollapsed(Widget))
		{
			Widget.State.DesiredSize = Desired;
			return Desired;
		}
		for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
		{
			ComputeDesired(*Child, Measurer);
		}

		switch (Widget.Type)
		{
		case EUIWidgetType::Canvas:
			// 점 앵커 자식이 차지하는 범위 (늘이기 축은 부모 크기를 따르므로 제외)
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				if (IsCollapsed(*Child))
				{
					continue;
				}
				const FUISlot& Slot = Child->Slot;
				if (Slot.IsAnchorPointX())
				{
					const float Width = Slot.bAutoSize ? Child->State.DesiredSize.X : Slot.Offsets.Right;
					Desired.X         = FMath::Max(Desired.X, Slot.Offsets.Left + Width * (1.0f - Slot.Alignment.X));
				}
				if (Slot.IsAnchorPointY())
				{
					const float Height = Slot.bAutoSize ? Child->State.DesiredSize.Y : Slot.Offsets.Bottom;
					Desired.Y          = FMath::Max(Desired.Y, Slot.Offsets.Top + Height * (1.0f - Slot.Alignment.Y));
				}
			}
			break;
		case EUIWidgetType::HorizontalBox:
		case EUIWidgetType::VerticalBox:
		case EUIWidgetType::ScrollBox:
		{
			const bool bHorizontal = Widget.Type == EUIWidgetType::HorizontalBox ||
			                         (Widget.Type == EUIWidgetType::ScrollBox && Widget.Orientation == EUIOrientation::Horizontal);
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				if (IsCollapsed(*Child))
				{
					continue;
				}
				const FVector2 Size = SlotDesired(*Child);
				if (bHorizontal)
				{
					Desired.X += Size.X;
					Desired.Y = FMath::Max(Desired.Y, Size.Y);
				}
				else
				{
					Desired.X = FMath::Max(Desired.X, Size.X);
					Desired.Y += Size.Y;
				}
			}
			break;
		}
		case EUIWidgetType::Overlay:
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				if (!IsCollapsed(*Child))
				{
					Desired = Max2(Desired, SlotDesired(*Child));
				}
			}
			break;
		case EUIWidgetType::UniformGrid:
		{
			FVector2 Cell = Widget.MinCellSize;
			int32    Rows = 0;
			int32    Cols = 0;
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				if (IsCollapsed(*Child))
				{
					continue;
				}
				Cell = Max2(Cell, SlotDesired(*Child));
				Rows = FMath::Max(Rows, FMath::Max(Child->Slot.Row, 0) + 1);
				Cols = FMath::Max(Cols, FMath::Max(Child->Slot.Column, 0) + 1);
			}
			Desired = FVector2(Cell.X * static_cast<float>(Cols), Cell.Y * static_cast<float>(Rows));
			break;
		}
		case EUIWidgetType::Border:
		case EUIWidgetType::Button:
			if (!Widget.Children.empty() && !IsCollapsed(*Widget.Children[0]))
			{
				Desired = SlotDesired(*Widget.Children[0]);
			}
			Desired += Widget.ContentPadding.GetTotal();
			break;
		case EUIWidgetType::Image:
			Desired = Widget.ImageSize;
			break;
		case EUIWidgetType::Text:
			Desired = Measurer.MeasureText(Widget, 0.0f);
			break;
		case EUIWidgetType::ProgressBar:
		case EUIWidgetType::Count:
			break;
		}

		Desired                  = Max2(Desired, Widget.MinSize);
		Widget.State.DesiredSize = Desired;
		return Desired;
	}

	// 너비가 정해졌을 때의 높이 (줄바꿈 텍스트만 너비에 따라 달라진다)
	float DesiredHeightForWidth(const FUIWidget& Child, float Width, IUITextMeasurer& Measurer)
	{
		if (Child.Type == EUIWidgetType::Text && Child.bWrap)
		{
			const float ContentWidth = FMath::Max(Width - Child.Slot.Padding.GetHorizontal(), 1.0f);
			return FMath::Max(Measurer.MeasureText(Child, ContentWidth).Y, Child.MinSize.Y);
		}
		return Child.State.DesiredSize.Y;
	}

	void Arrange(FUIWidget& Widget, const FUIRect& Rect, const FUIRect& Clip, IUITextMeasurer& Measurer);

	// 가로/세로 나열 (Start = 주축 시작 위치, 스크롤은 오프셋만큼 당긴다). bUseSizeRule이 false면 모두 Auto
	void ArrangeStack(FUIWidget& Widget, const FUIRect& Rect, bool bHorizontal, bool bUseSizeRule, float ScrollOffset, const FUIRect& ChildClip,
	                  IUITextMeasurer& Measurer)
	{
		const float CrossSize = bHorizontal ? Rect.GetHeight() : Rect.GetWidth();
		const float MainSize  = bHorizontal ? Rect.GetWidth() : Rect.GetHeight();

		// 주축 원하는 크기 (여백 포함)
		std::vector<float> MainDesired(Widget.Children.size(), 0.0f);
		float              AutoTotal   = 0.0f;
		float              FillWeights = 0.0f;
		for (size_t Index = 0; Index < Widget.Children.size(); ++Index)
		{
			const FUIWidget& Child = *Widget.Children[Index];
			if (IsCollapsed(Child))
			{
				continue;
			}
			const bool bFill = bUseSizeRule && Child.Slot.SizeRule == EUISizeRule::Fill;
			if (bHorizontal)
			{
				MainDesired[Index] = Child.State.DesiredSize.X + Child.Slot.Padding.GetHorizontal();
			}
			else
			{
				MainDesired[Index] = DesiredHeightForWidth(Child, CrossSize, Measurer) + Child.Slot.Padding.GetVertical();
			}
			if (bFill)
			{
				FillWeights += FMath::Max(Child.Slot.FillWeight, 0.0f);
				AutoTotal += bHorizontal ? Child.Slot.Padding.GetHorizontal() : Child.Slot.Padding.GetVertical();
			}
			else
			{
				AutoTotal += MainDesired[Index];
			}
		}
		const float Remaining = FMath::Max(MainSize - AutoTotal, 0.0f);

		float Cursor = (bHorizontal ? Rect.Min.X : Rect.Min.Y) - ScrollOffset;
		for (size_t Index = 0; Index < Widget.Children.size(); ++Index)
		{
			FUIWidget& Child = *Widget.Children[Index];
			if (IsCollapsed(Child))
			{
				continue;
			}
			const bool bFill   = bUseSizeRule && Child.Slot.SizeRule == EUISizeRule::Fill;
			float      SlotLen = MainDesired[Index];
			if (bFill)
			{
				const float Padding = bHorizontal ? Child.Slot.Padding.GetHorizontal() : Child.Slot.Padding.GetVertical();
				const float Share   = FillWeights > 0.0f ? Remaining * FMath::Max(Child.Slot.FillWeight, 0.0f) / FillWeights : 0.0f;
				SlotLen             = Share + Padding;
			}

			FUIRect Slot;
			if (bHorizontal)
			{
				Slot = FUIRect(FVector2(Cursor, Rect.Min.Y), FVector2(Cursor + SlotLen, Rect.Max.Y));
			}
			else
			{
				Slot = FUIRect(FVector2(Rect.Min.X, Cursor), FVector2(Rect.Max.X, Cursor + SlotLen));
			}
			FVector2 Desired = Child.State.DesiredSize;
			if (!bHorizontal)
			{
				Desired.Y = SlotLen - Child.Slot.Padding.GetVertical();
			}
			// 주축은 슬롯을 채우고(자동 슬롯은 원하는 크기와 같다) 교차축만 정렬을 따른다
			const EUIHAlign HAlign = bHorizontal ? EUIHAlign::Fill : Child.Slot.HAlign;
			const EUIVAlign VAlign = bHorizontal ? Child.Slot.VAlign : EUIVAlign::Fill;
			Arrange(Child, FUILayout::AlignInRect(Slot, Child.Slot.Padding, Desired, HAlign, VAlign), ChildClip, Measurer);
			Cursor += SlotLen;
		}
	}

	void Arrange(FUIWidget& Widget, const FUIRect& Rect, const FUIRect& Clip, IUITextMeasurer& Measurer)
	{
		Widget.State.Geometry = Rect;
		Widget.State.Clip     = Clip;
		if (IsCollapsed(Widget))
		{
			return;
		}

		switch (Widget.Type)
		{
		case EUIWidgetType::Canvas:
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				Arrange(*Child, FUILayout::ArrangeCanvasSlot(Rect, Child->Slot, Child->State.DesiredSize), Clip, Measurer);
			}
			break;
		case EUIWidgetType::HorizontalBox:
		case EUIWidgetType::VerticalBox:
			ArrangeStack(Widget, Rect, Widget.Type == EUIWidgetType::HorizontalBox, true, 0.0f, Clip, Measurer);
			break;
		case EUIWidgetType::Overlay:
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				Arrange(*Child, FUILayout::AlignInRect(Rect, Child->Slot.Padding, Child->State.DesiredSize, Child->Slot.HAlign, Child->Slot.VAlign), Clip,
				        Measurer);
			}
			break;
		case EUIWidgetType::UniformGrid:
		{
			int32 Rows = 1;
			int32 Cols = 1;
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				if (!IsCollapsed(*Child))
				{
					Rows = FMath::Max(Rows, FMath::Max(Child->Slot.Row, 0) + 1);
					Cols = FMath::Max(Cols, FMath::Max(Child->Slot.Column, 0) + 1);
				}
			}
			const FVector2 Cell(Rect.GetWidth() / static_cast<float>(Cols), Rect.GetHeight() / static_cast<float>(Rows));
			for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
			{
				const FVector2 CellMin = Rect.Min + FVector2(Cell.X * static_cast<float>(FMath::Max(Child->Slot.Column, 0)),
				                                             Cell.Y * static_cast<float>(FMath::Max(Child->Slot.Row, 0)));
				Arrange(*Child,
				        FUILayout::AlignInRect(FUIRect(CellMin, CellMin + Cell), Child->Slot.Padding, Child->State.DesiredSize, Child->Slot.HAlign,
				                               Child->Slot.VAlign),
				        Clip, Measurer);
			}
			break;
		}
		case EUIWidgetType::ScrollBox:
		{
			const bool  bHorizontal = Widget.Orientation == EUIOrientation::Horizontal;
			const float Content     = bHorizontal ? Widget.State.DesiredSize.X : 0.0f;
			float       ContentLen  = Content;
			if (!bHorizontal)
			{
				// 세로: 줄바꿈 텍스트 높이는 폭에 따라 달라지므로 다시 합산
				for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
				{
					if (!IsCollapsed(*Child))
					{
						ContentLen += DesiredHeightForWidth(*Child, Rect.GetWidth(), Measurer) + Child->Slot.Padding.GetVertical();
					}
				}
			}
			const float ViewLen      = bHorizontal ? Rect.GetWidth() : Rect.GetHeight();
			Widget.State.ScrollMax    = FMath::Max(ContentLen - ViewLen, 0.0f);
			Widget.State.ScrollOffset = FMath::Clamp(Widget.State.ScrollOffset, 0.0f, Widget.State.ScrollMax);
			// 스크롤이 생기면 막대 폭만큼 내용 영역을 줄인다
			FUIRect ContentRect = Rect;
			if (Widget.State.ScrollMax > 0.0f)
			{
				if (bHorizontal)
				{
					ContentRect.Max.Y = FMath::Max(ContentRect.Max.Y - Widget.ScrollbarWidth, ContentRect.Min.Y);
				}
				else
				{
					ContentRect.Max.X = FMath::Max(ContentRect.Max.X - Widget.ScrollbarWidth, ContentRect.Min.X);
				}
			}
			ArrangeStack(Widget, ContentRect, bHorizontal, false, Widget.State.ScrollOffset, Clip.Intersect(Rect), Measurer);
			break;
		}
		case EUIWidgetType::Border:
		case EUIWidgetType::Button:
			if (!Widget.Children.empty())
			{
				FUIWidget& Child = *Widget.Children[0];
				Arrange(Child,
				        FUILayout::AlignInRect(Rect.Inset(Widget.ContentPadding), Child.Slot.Padding, Child.State.DesiredSize, Child.Slot.HAlign,
				                               Child.Slot.VAlign),
				        Clip, Measurer);
			}
			break;
		default:
			break;
		}
	}
} // namespace

void FUILayout::Compute(FUIWidget& Root, const FVector2& RootSize, IUITextMeasurer& Measurer)
{
	ComputeDesired(Root, Measurer);
	Arrange(Root, FUIRect(FVector2::ZeroVector, RootSize), FUIRect::Infinite(), Measurer);
}

float FUILayout::ComputeScale(EUIScaleMode Mode, const FVector2& DesignSize, const FVector2& ViewportSize)
{
	if (DesignSize.X <= 0.0f || DesignSize.Y <= 0.0f || ViewportSize.X <= 0.0f || ViewportSize.Y <= 0.0f)
	{
		return 1.0f;
	}
	const float ScaleX = ViewportSize.X / DesignSize.X;
	const float ScaleY = ViewportSize.Y / DesignSize.Y;
	switch (Mode)
	{
	case EUIScaleMode::MatchHeight: return ScaleY;
	case EUIScaleMode::MatchWidth:  return ScaleX;
	case EUIScaleMode::Fit:         return FMath::Min(ScaleX, ScaleY);
	case EUIScaleMode::Fill:        return FMath::Max(ScaleX, ScaleY);
	default:                        return 1.0f;
	}
}

FUIRect FUILayout::ArrangeCanvasSlot(const FUIRect& Parent, const FUISlot& Slot, const FVector2& DesiredSize)
{
	const FVector2 ParentSize = Parent.GetSize();
	FUIRect        Result;
	if (Slot.IsAnchorPointX())
	{
		const float Anchor = Parent.Min.X + Slot.AnchorMin.X * ParentSize.X;
		const float Width  = Slot.bAutoSize ? DesiredSize.X : Slot.Offsets.Right;
		Result.Min.X       = Anchor + Slot.Offsets.Left - Slot.Alignment.X * Width;
		Result.Max.X       = Result.Min.X + Width;
	}
	else
	{
		Result.Min.X = Parent.Min.X + Slot.AnchorMin.X * ParentSize.X + Slot.Offsets.Left;
		Result.Max.X = Parent.Min.X + Slot.AnchorMax.X * ParentSize.X - Slot.Offsets.Right;
	}
	if (Slot.IsAnchorPointY())
	{
		const float Anchor = Parent.Min.Y + Slot.AnchorMin.Y * ParentSize.Y;
		const float Height = Slot.bAutoSize ? DesiredSize.Y : Slot.Offsets.Bottom;
		Result.Min.Y       = Anchor + Slot.Offsets.Top - Slot.Alignment.Y * Height;
		Result.Max.Y       = Result.Min.Y + Height;
	}
	else
	{
		Result.Min.Y = Parent.Min.Y + Slot.AnchorMin.Y * ParentSize.Y + Slot.Offsets.Top;
		Result.Max.Y = Parent.Min.Y + Slot.AnchorMax.Y * ParentSize.Y - Slot.Offsets.Bottom;
	}
	Result.Max.X = FMath::Max(Result.Max.X, Result.Min.X);
	Result.Max.Y = FMath::Max(Result.Max.Y, Result.Min.Y);
	return Result;
}

FUIRect FUILayout::AlignInRect(const FUIRect& Area, const FUIMargin& Padding, const FVector2& DesiredSize, EUIHAlign HAlign, EUIVAlign VAlign)
{
	const FUIRect Inner = Area.Inset(Padding);
	FUIRect       Result;
	switch (HAlign)
	{
	case EUIHAlign::Left:
		Result.Min.X = Inner.Min.X;
		Result.Max.X = Inner.Min.X + DesiredSize.X;
		break;
	case EUIHAlign::Center:
		Result.Min.X = Inner.Min.X + (Inner.GetWidth() - DesiredSize.X) * 0.5f;
		Result.Max.X = Result.Min.X + DesiredSize.X;
		break;
	case EUIHAlign::Right:
		Result.Min.X = Inner.Max.X - DesiredSize.X;
		Result.Max.X = Inner.Max.X;
		break;
	default:
		Result.Min.X = Inner.Min.X;
		Result.Max.X = Inner.Max.X;
		break;
	}
	switch (VAlign)
	{
	case EUIVAlign::Top:
		Result.Min.Y = Inner.Min.Y;
		Result.Max.Y = Inner.Min.Y + DesiredSize.Y;
		break;
	case EUIVAlign::Center:
		Result.Min.Y = Inner.Min.Y + (Inner.GetHeight() - DesiredSize.Y) * 0.5f;
		Result.Max.Y = Result.Min.Y + DesiredSize.Y;
		break;
	case EUIVAlign::Bottom:
		Result.Min.Y = Inner.Max.Y - DesiredSize.Y;
		Result.Max.Y = Inner.Max.Y;
		break;
	default:
		Result.Min.Y = Inner.Min.Y;
		Result.Max.Y = Inner.Max.Y;
		break;
	}
	return Result;
}

void FUILayout::MoveCanvasSlot(FUISlot& Slot, const FVector2& Delta)
{
	Slot.Offsets.Left += Delta.X;
	if (!Slot.IsAnchorPointX())
	{
		Slot.Offsets.Right -= Delta.X;
	}
	Slot.Offsets.Top += Delta.Y;
	if (!Slot.IsAnchorPointY())
	{
		Slot.Offsets.Bottom -= Delta.Y;
	}
}

void FUILayout::SetCanvasSlotRect(FUISlot& Slot, const FUIRect& Parent, const FUIRect& Desired)
{
	const FVector2 ParentSize = Parent.GetSize();
	if (Slot.IsAnchorPointX())
	{
		const float Width  = Desired.GetWidth();
		const float Anchor = Parent.Min.X + Slot.AnchorMin.X * ParentSize.X;
		if (!Slot.bAutoSize)
		{
			Slot.Offsets.Right = Width;
		}
		Slot.Offsets.Left = Desired.Min.X - Anchor + Slot.Alignment.X * Width;
	}
	else
	{
		Slot.Offsets.Left  = Desired.Min.X - (Parent.Min.X + Slot.AnchorMin.X * ParentSize.X);
		Slot.Offsets.Right = (Parent.Min.X + Slot.AnchorMax.X * ParentSize.X) - Desired.Max.X;
	}
	if (Slot.IsAnchorPointY())
	{
		const float Height = Desired.GetHeight();
		const float Anchor = Parent.Min.Y + Slot.AnchorMin.Y * ParentSize.Y;
		if (!Slot.bAutoSize)
		{
			Slot.Offsets.Bottom = Height;
		}
		Slot.Offsets.Top = Desired.Min.Y - Anchor + Slot.Alignment.Y * Height;
	}
	else
	{
		Slot.Offsets.Top    = Desired.Min.Y - (Parent.Min.Y + Slot.AnchorMin.Y * ParentSize.Y);
		Slot.Offsets.Bottom = (Parent.Min.Y + Slot.AnchorMax.Y * ParentSize.Y) - Desired.Max.Y;
	}
}

void FUILayout::SetAnchorsKeepRect(FUISlot& Slot, const FUIRect& Parent, const FUIRect& Current, const FVector2& AnchorMin, const FVector2& AnchorMax)
{
	Slot.AnchorMin = AnchorMin;
	Slot.AnchorMax = AnchorMax;
	// 늘이기 축은 자동 크기가 의미 없다
	if (!Slot.IsAnchorPointX() && !Slot.IsAnchorPointY())
	{
		Slot.bAutoSize = false;
	}
	const bool bAutoSize = Slot.bAutoSize;
	Slot.bAutoSize       = false; // 현재 크기를 오프셋에 기록
	SetCanvasSlotRect(Slot, Parent, Current);
	Slot.bAutoSize = bAutoSize;
}

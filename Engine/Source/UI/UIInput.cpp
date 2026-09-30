#include "UI/UIInput.h"

#include "UI/Widget.h"

#include <algorithm>

namespace
{
	// 그리기 순서의 역순(위에 있는 것 먼저)으로 자식 목록
	void GetChildrenTopFirst(const FUIWidget& Widget, std::vector<FUIWidget*>& Out)
	{
		Out.clear();
		for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
		{
			Out.push_back(Child.get());
		}
		if (Widget.Type == EUIWidgetType::Canvas)
		{
			std::stable_sort(Out.begin(), Out.end(), [](const FUIWidget* A, const FUIWidget* B) { return A->Slot.ZOrder < B->Slot.ZOrder; });
		}
		std::reverse(Out.begin(), Out.end());
	}

	FUIWidget* HitTestRecursive(FUIWidget& Widget, const FVector2& Point)
	{
		switch (Widget.Visibility)
		{
		case EUIVisibility::Collapsed:
		case EUIVisibility::Hidden:
		case EUIVisibility::HitTestInvisible: return nullptr;
		default:                              break;
		}
		if (!Widget.State.Clip.Contains(Point))
		{
			return nullptr;
		}
		std::vector<FUIWidget*> Children;
		GetChildrenTopFirst(Widget, Children);
		for (FUIWidget* Child : Children)
		{
			if (FUIWidget* Hit = HitTestRecursive(*Child, Point))
			{
				return Hit;
			}
		}
		if (Widget.Visibility == EUIVisibility::Visible && Widget.State.Geometry.Contains(Point))
		{
			return &Widget;
		}
		return nullptr;
	}

	FUIWidget* FindAncestorOfType(FUIWidget* Widget, EUIWidgetType Type)
	{
		for (FUIWidget* Current = Widget; Current != nullptr; Current = Current->Parent)
		{
			if (Current->Type == Type)
			{
				return Current;
			}
		}
		return nullptr;
	}

	// 키보드 포커스 후보: 보이고 켜진 버튼 (트리 순서)
	void CollectFocusable(FUIWidget& Widget, std::vector<FUIWidget*>& Out)
	{
		if (Widget.Visibility == EUIVisibility::Collapsed || Widget.Visibility == EUIVisibility::Hidden ||
		    Widget.Visibility == EUIVisibility::HitTestInvisible || !Widget.bEnabled)
		{
			return;
		}
		if (Widget.Type == EUIWidgetType::Button && Widget.Visibility == EUIVisibility::Visible)
		{
			Out.push_back(&Widget);
		}
		for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
		{
			CollectFocusable(*Child, Out);
		}
	}

	void Emit(std::vector<FUIEvent>& Out, EUIEventType Type, const FUIWidget& Widget)
	{
		Out.push_back({ Type, Widget.State.Id, Widget.Name });
	}
} // namespace

FUIWidget* FUIInputRouter::HitTest(FUIWidget& Root, const FVector2& Point)
{
	return HitTestRecursive(Root, Point);
}

void FUIInputRouter::SetFocus(FUIWidget& Root, uint32 WidgetId)
{
	if (FUIWidget* Old = FocusedId != 0 ? Root.FindById(FocusedId) : nullptr)
	{
		Old->State.bFocused = false;
	}
	FocusedId = 0;
	if (FUIWidget* New = WidgetId != 0 ? Root.FindById(WidgetId) : nullptr)
	{
		New->State.bFocused = true;
		FocusedId           = WidgetId;
	}
}

void FUIInputRouter::Reset(FUIWidget& Root)
{
	Root.ForEach([](FUIWidget& Widget) {
		Widget.State.bHovered = false;
		Widget.State.bPressed = false;
		Widget.State.bFocused = false;
	});
	HoveredId = PressedId = FocusedId = 0;
}

bool FUIInputRouter::Process(FUIWidget& Root, const FUIPointerInput& Pointer, const FUIKeyInput& Keys, std::vector<FUIEvent>& OutEvents)
{
	const bool bCaptured = PressedId != 0; // 버튼을 누른 채 밖으로 끌어도 뗄 때까지 포인터를 가져간다
	FUIWidget* Hit       = Pointer.bInside ? HitTest(Root, Pointer.Position) : nullptr;
	FUIWidget* Button = FindAncestorOfType(Hit, EUIWidgetType::Button);
	if (Button != nullptr && !Button->IsEnabledInHierarchy())
	{
		Button = nullptr; // 비활성 버튼은 입력을 막기만 하고 반응하지 않는다
	}

	// 호버 (버튼만 상태를 가진다)
	const uint32 NewHoveredId = Button != nullptr ? Button->State.Id : 0;
	if (NewHoveredId != HoveredId)
	{
		if (FUIWidget* Old = HoveredId != 0 ? Root.FindById(HoveredId) : nullptr)
		{
			Old->State.bHovered = false;
			Emit(OutEvents, EUIEventType::HoverEnd, *Old);
		}
		if (Button != nullptr)
		{
			Button->State.bHovered = true;
			Emit(OutEvents, EUIEventType::HoverBegin, *Button);
		}
		HoveredId = NewHoveredId;
	}

	// 누르기 / 떼기
	if (Pointer.bPressed)
	{
		if (Button != nullptr)
		{
			PressedId              = Button->State.Id;
			Button->State.bPressed = true;
			Emit(OutEvents, EUIEventType::Pressed, *Button);
			SetFocus(Root, Button->State.Id);
		}
		else if (Hit != nullptr || Pointer.bInside)
		{
			SetFocus(Root, 0);
		}
	}
	if (Pointer.bReleased || (!Pointer.bDown && PressedId != 0 && !Pointer.bPressed))
	{
		if (FUIWidget* Pressed = PressedId != 0 ? Root.FindById(PressedId) : nullptr)
		{
			Pressed->State.bPressed = false;
			Emit(OutEvents, EUIEventType::Released, *Pressed);
			if (Pointer.bReleased && Pressed == Button)
			{
				Emit(OutEvents, EUIEventType::Clicked, *Pressed);
			}
		}
		PressedId = 0;
	}

	// 휠 → 가장 가까운 스크롤 박스
	if (Pointer.Wheel != 0.0f)
	{
		if (FUIWidget* Scroll = FindAncestorOfType(Hit, EUIWidgetType::ScrollBox))
		{
			Scroll->State.ScrollOffset = FMath::Clamp(Scroll->State.ScrollOffset - Pointer.Wheel * WheelStep, 0.0f, Scroll->State.ScrollMax);
		}
	}

	// 키보드: Tab 이동, Enter/Space 실행
	if (Keys.bFocusNext || Keys.bFocusPrevious)
	{
		std::vector<FUIWidget*> Focusable;
		CollectFocusable(Root, Focusable);
		if (!Focusable.empty())
		{
			const auto It      = std::find_if(Focusable.begin(), Focusable.end(), [this](const FUIWidget* Widget) { return Widget->State.Id == FocusedId; });
			const int32 Count  = static_cast<int32>(Focusable.size());
			int32       Index  = It == Focusable.end() ? (Keys.bFocusNext ? -1 : 0) : static_cast<int32>(It - Focusable.begin());
			Index              = (Index + (Keys.bFocusNext ? 1 : -1) + Count) % Count;
			SetFocus(Root, Focusable[static_cast<size_t>(Index)]->State.Id);
		}
	}
	if (Keys.bActivate && FocusedId != 0)
	{
		FUIWidget* Focused = Root.FindById(FocusedId);
		if (Focused != nullptr && Focused->IsEnabledInHierarchy() && Focused->Visibility == EUIVisibility::Visible)
		{
			Emit(OutEvents, EUIEventType::Clicked, *Focused);
		}
	}
	return Hit != nullptr || bCaptured;
}

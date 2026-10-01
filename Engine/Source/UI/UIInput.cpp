#include "UI/UIInput.h"

#include "UI/UIFont.h"
#include "UI/UITextEdit.h"
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
		if (!Widget.State.VisualClip.Contains(Point))
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
		if (Widget.Visibility == EUIVisibility::Visible && Widget.State.VisualGeometry.Contains(Point))
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
		if ((Widget.Type == EUIWidgetType::Button || Widget.Type == EUIWidgetType::TextBox) && Widget.Visibility == EUIVisibility::Visible)
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

	// 텍스트 상자 안 X(UI 단위) → 가장 가까운 글자 경계
	int32 CaretFromX(const FUIWidget& Box, float PointerX)
	{
		FUIFont* Font = FUIFontLibrary::Get().GetFont(Box.Font);
		if (Font == nullptr)
		{
			return 0;
		}
		std::vector<float> Positions;
		Font->GetCaretPositions(Box.Text, Box.FontSize, Positions);
		const float LayoutX = (PointerX - Box.State.VisualOffset.X) / Box.State.VisualScale.X; // 렌더 변환 역변환
		const float Local   = LayoutX - (Box.State.Geometry.Min.X + Box.ContentPadding.Left) + Box.State.TextScroll;
		int32       Best  = 0;
		for (size_t Index = 1; Index < Positions.size(); ++Index)
		{
			if (FMath::Abs(Positions[Index] - Local) < FMath::Abs(Positions[static_cast<size_t>(Best)] - Local))
			{
				Best = static_cast<int32>(Index);
			}
		}
		return Best;
	}

	// 포커스된 텍스트 상자 편집 (FUITextEdit). 반환: 내용이 바뀜
	bool EditText(FUIWidget& Box, const FUIKeyInput& Keys, std::string* OutClipboard)
	{
		FUITextEdit Edit;
		Edit.Text     = DecodeUtf8(Box.Text);
		Edit.Caret    = FMath::Clamp(Box.State.CaretIndex, 0, static_cast<int32>(Edit.Text.size()));
		Edit.Anchor   = Box.State.SelectionAnchor;
		bool bMoved   = false;
		bool bChanged = Edit.Apply(Keys, Box.MaxLength, OutClipboard, &bMoved);
		// IME 조합이 시작되면 선택 영역은 지운다 (조합 글자가 그 자리에 들어간다)
		if (!Keys.Composition.empty() && Edit.HasSelection())
		{
			bChanged |= Edit.DeleteSelection();
		}
		Box.State.CaretIndex      = Edit.Caret;
		Box.State.SelectionAnchor = Edit.Anchor;
		if (Box.State.Composition != Keys.Composition || Box.State.CompositionCursor != Keys.CompositionCursor)
		{
			Box.State.Composition       = Keys.Composition;
			Box.State.CompositionCursor = Keys.CompositionCursor;
			bMoved                      = true;
		}
		if (bChanged)
		{
			Box.Text = EncodeUtf8(Edit.Text);
		}
		if (bChanged || bMoved)
		{
			Box.State.CaretTime = 0.0f; // 편집 중에는 캐럿이 보이게
		}
		return bChanged;
	}

	void ClearTextSelection(FUIWidget& Box)
	{
		Box.State.SelectionAnchor = -1;
		Box.State.Composition.clear();
		Box.State.CompositionCursor = 0;
	}
} // namespace

bool FUIInputRouter::WantsKeyboard(FUIWidget& Root)
{
	const FUIWidget* Focused = FocusedId != 0 ? Root.FindById(FocusedId) : nullptr;
	return Focused != nullptr && Focused->Type == EUIWidgetType::TextBox;
}

FUIWidget* FUIInputRouter::HitTest(FUIWidget& Root, const FVector2& Point)
{
	return HitTestRecursive(Root, Point);
}

void FUIInputRouter::SetFocus(FUIWidget& Root, uint32 WidgetId)
{
	if (FUIWidget* Old = FocusedId != 0 ? Root.FindById(FocusedId) : nullptr)
	{
		Old->State.bFocused = false;
		ClearTextSelection(*Old);
	}
	FocusedId   = 0;
	SelectingId = 0;
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
		ClearTextSelection(Widget);
	});
	HoveredId = PressedId = FocusedId = SelectingId = 0;
	ClipboardOut.clear();
	bClipboardOut = false;
}

bool FUIInputRouter::TakeClipboardText(std::string& Out)
{
	if (!bClipboardOut)
	{
		return false;
	}
	Out           = std::move(ClipboardOut);
	ClipboardOut  = {};
	bClipboardOut = false;
	return true;
}

bool FUIInputRouter::Process(FUIWidget& Root, const FUIPointerInput& Pointer, const FUIKeyInput& Keys, std::vector<FUIEvent>& OutEvents)
{
	const bool bCaptured = PressedId != 0 || SelectingId != 0; // 버튼을 누른 채(글자 선택 중) 밖으로 끌어도 뗄 때까지 포인터를 가져간다
	FUIWidget* Hit       = Pointer.bInside ? HitTest(Root, Pointer.Position) : nullptr;
	FUIWidget* Button    = FindAncestorOfType(Hit, EUIWidgetType::Button);
	if (Button != nullptr && !Button->IsEnabledInHierarchy())
	{
		Button = nullptr; // 비활성 버튼은 입력을 막기만 하고 반응하지 않는다
	}
	FUIWidget* TextBox = FindAncestorOfType(Hit, EUIWidgetType::TextBox);
	if (TextBox != nullptr && !TextBox->IsEnabledInHierarchy())
	{
		TextBox = nullptr;
	}
	// 포커스를 옮길 때 떠나는 텍스트 상자는 확정 이벤트
	const auto ChangeFocus = [&](uint32 NewId) {
		if (NewId == FocusedId)
		{
			return;
		}
		if (FUIWidget* Old = FocusedId != 0 ? Root.FindById(FocusedId) : nullptr; Old != nullptr && Old->Type == EUIWidgetType::TextBox)
		{
			Emit(OutEvents, EUIEventType::TextCommitted, *Old);
		}
		SetFocus(Root, NewId);
	};

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
			ChangeFocus(Button->State.Id);
		}
		else if (TextBox != nullptr)
		{
			// 클릭 = 캐럿 이동, Shift+클릭 = 선택 넓히기, 누른 채 끌면 선택 (뗄 때까지 포인터를 가져간다)
			const bool  bSameBox  = FocusedId == TextBox->State.Id;
			const int32 OldCaret  = TextBox->State.CaretIndex;
			ChangeFocus(TextBox->State.Id);
			TextBox->State.CaretIndex = CaretFromX(*TextBox, Pointer.Position.X);
			if (Keys.bShift && bSameBox)
			{
				TextBox->State.SelectionAnchor = TextBox->State.SelectionAnchor >= 0 ? TextBox->State.SelectionAnchor : OldCaret;
			}
			else
			{
				TextBox->State.SelectionAnchor = TextBox->State.CaretIndex;
			}
			TextBox->State.CaretTime = 0.0f;
			SelectingId              = TextBox->State.Id;
			SelectingAnchor          = TextBox->State.SelectionAnchor;
		}
		else if (Hit != nullptr || Pointer.bInside)
		{
			ChangeFocus(0);
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
	// 텍스트 상자 끌어 선택
	if (SelectingId != 0)
	{
		FUIWidget* Selecting = Root.FindById(SelectingId);
		if (Selecting != nullptr && SelectingId == FocusedId)
		{
			if (Pointer.bDown && !Pointer.bPressed)
			{
				const int32 Caret = CaretFromX(*Selecting, Pointer.Position.X);
				if (Caret != Selecting->State.CaretIndex)
				{
					Selecting->State.CaretIndex = Caret;
					Selecting->State.CaretTime  = 0.0f;
				}
				Selecting->State.SelectionAnchor = Caret != SelectingAnchor ? SelectingAnchor : -1; // 누른 자리 ~ 지금 자리
			}
		}
		if (!Pointer.bDown || Selecting == nullptr)
		{
			SelectingId = 0;
		}
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
			ChangeFocus(Focusable[static_cast<size_t>(Index)]->State.Id);
			if (FUIWidget* Focused = Root.FindById(FocusedId); Focused != nullptr && Focused->Type == EUIWidgetType::TextBox)
			{
				Focused->State.CaretIndex = static_cast<int32>(DecodeUtf8(Focused->Text).size()); // Tab으로 들어오면 끝에
			}
		}
	}
	FUIWidget* Focused = FocusedId != 0 ? Root.FindById(FocusedId) : nullptr;
	if (Focused != nullptr && Focused->Type == EUIWidgetType::TextBox)
	{
		// 텍스트 상자: 입력/지우기/이동, Enter = 확정 후 포커스 해제, Esc = 포커스 해제
		if (!Focused->IsEnabledInHierarchy() || Focused->Visibility != EUIVisibility::Visible)
		{
			ChangeFocus(0);
		}
		else if (Keys.bCancel)
		{
			SetFocus(Root, 0);
		}
		else
		{
			std::string Copied;
			const bool  bEdited = EditText(*Focused, Keys, &Copied);
			if (!Copied.empty())
			{
				ClipboardOut  = std::move(Copied);
				bClipboardOut = true;
			}
			if (bEdited)
			{
				Emit(OutEvents, EUIEventType::TextChanged, *Focused);
			}
			if (Keys.bCommit)
			{
				ChangeFocus(0); // 확정 이벤트
			}
		}
	}
	else if (Keys.bActivate && Focused != nullptr)
	{
		if (Focused->IsEnabledInHierarchy() && Focused->Visibility == EUIVisibility::Visible)
		{
			Emit(OutEvents, EUIEventType::Clicked, *Focused);
		}
	}
	return Hit != nullptr || bCaptured;
}

#include "UI/Widget.h"

#include <algorithm>

namespace
{
	FUIBrush MakeBrush(const FVector4& Color, float CornerRadius)
	{
		FUIBrush Brush;
		Brush.Color        = Color;
		Brush.CornerRadius = CornerRadius;
		return Brush;
	}
} // namespace

EUIVisibility FUIWidget::GetDefaultVisibility(EUIWidgetType Type)
{
	switch (Type)
	{
	case EUIWidgetType::Canvas:
	case EUIWidgetType::HorizontalBox:
	case EUIWidgetType::VerticalBox:
	case EUIWidgetType::Overlay:
	case EUIWidgetType::UniformGrid: return EUIVisibility::SelfHitTestInvisible; // 배치용 패널은 빈 곳의 클릭을 게임으로 흘려보낸다
	case EUIWidgetType::Text:
	case EUIWidgetType::ProgressBar: return EUIVisibility::HitTestInvisible;
	default:                         return EUIVisibility::Visible; // 스크롤 박스는 휠을 받는다
	}
}

std::unique_ptr<FUIWidget> FUIWidget::Create(EUIWidgetType Type)
{
	auto Widget        = std::make_unique<FUIWidget>(Type);
	Widget->Name       = ToString(Type);
	Widget->Visibility = GetDefaultVisibility(Type);
	switch (Type)
	{
	case EUIWidgetType::Border:
		Widget->Brush          = MakeBrush(FVector4(0.08f, 0.09f, 0.12f, 0.85f), 6.0f);
		Widget->ContentPadding = FUIMargin(8.0f);
		break;
	case EUIWidgetType::Text:
		Widget->Text = "텍스트";
		break;
	case EUIWidgetType::Button:
		Widget->Brush          = MakeBrush(FVector4(0.22f, 0.45f, 0.9f, 1.0f), 6.0f);
		Widget->HoveredBrush   = MakeBrush(FVector4(0.35f, 0.58f, 1.0f, 1.0f), 6.0f);
		Widget->PressedBrush   = MakeBrush(FVector4(0.14f, 0.32f, 0.7f, 1.0f), 6.0f);
		Widget->DisabledBrush  = MakeBrush(FVector4(0.35f, 0.35f, 0.38f, 1.0f), 6.0f);
		Widget->ContentPadding = FUIMargin(16.0f, 8.0f);
		break;
	case EUIWidgetType::ProgressBar:
		Widget->Brush     = MakeBrush(FVector4(0.06f, 0.06f, 0.07f, 0.85f), 4.0f);
		Widget->FillBrush = MakeBrush(FVector4(0.3f, 0.85f, 0.4f, 1.0f), 4.0f);
		Widget->MinSize   = FVector2(100.0f, 16.0f);
		break;
	default:
		break; // 배치 패널/이미지는 기본값 그대로
	}
	return Widget;
}

std::unique_ptr<FUIWidget> FUIWidget::Clone() const
{
	auto Copy                            = std::make_unique<FUIWidget>();
	static_cast<FUIWidgetData&>(*Copy)   = static_cast<const FUIWidgetData&>(*this);
	Copy->Children.reserve(Children.size());
	for (const std::unique_ptr<FUIWidget>& Child : Children)
	{
		Copy->AddChild(Child->Clone());
	}
	return Copy;
}

FUIWidget* FUIWidget::AddChild(std::unique_ptr<FUIWidget> Child, int32 Index)
{
	FUIWidget* Raw = Child.get();
	Raw->Parent    = this;
	if (Index < 0 || Index >= static_cast<int32>(Children.size()))
	{
		Children.push_back(std::move(Child));
	}
	else
	{
		Children.insert(Children.begin() + Index, std::move(Child));
	}
	return Raw;
}

std::unique_ptr<FUIWidget> FUIWidget::RemoveChild(const FUIWidget* Child)
{
	const int32 Index = GetChildIndex(Child);
	if (Index < 0)
	{
		return nullptr;
	}
	std::unique_ptr<FUIWidget> Removed = std::move(Children[static_cast<size_t>(Index)]);
	Children.erase(Children.begin() + Index);
	Removed->Parent = nullptr;
	return Removed;
}

int32 FUIWidget::GetChildIndex(const FUIWidget* Child) const
{
	for (size_t Index = 0; Index < Children.size(); ++Index)
	{
		if (Children[Index].get() == Child)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

int32 FUIWidget::GetMaxChildren(EUIWidgetType Type)
{
	switch (Type)
	{
	case EUIWidgetType::Canvas:
	case EUIWidgetType::HorizontalBox:
	case EUIWidgetType::VerticalBox:
	case EUIWidgetType::Overlay:
	case EUIWidgetType::UniformGrid:
	case EUIWidgetType::ScrollBox:   return -1;
	case EUIWidgetType::Border:
	case EUIWidgetType::Button:      return 1;
	default:                         return 0;
	}
}

bool FUIWidget::CanAddChild() const
{
	const int32 Max = GetMaxChildren(Type);
	return Max < 0 || static_cast<int32>(Children.size()) < Max;
}

bool FUIWidget::IsAncestorOf(const FUIWidget* Other) const
{
	for (const FUIWidget* Current = Other != nullptr ? Other->Parent : nullptr; Current != nullptr; Current = Current->Parent)
	{
		if (Current == this)
		{
			return true;
		}
	}
	return false;
}

FUIWidget* FUIWidget::FindByName(std::string_view InName)
{
	return const_cast<FUIWidget*>(static_cast<const FUIWidget*>(this)->FindByName(InName));
}

const FUIWidget* FUIWidget::FindByName(std::string_view InName) const
{
	if (Name == InName)
	{
		return this;
	}
	for (const std::unique_ptr<FUIWidget>& Child : Children)
	{
		if (const FUIWidget* Found = Child->FindByName(InName))
		{
			return Found;
		}
	}
	return nullptr;
}

FUIWidget* FUIWidget::FindById(uint32 Id)
{
	if (State.Id == Id)
	{
		return this;
	}
	for (const std::unique_ptr<FUIWidget>& Child : Children)
	{
		if (FUIWidget* Found = Child->FindById(Id))
		{
			return Found;
		}
	}
	return nullptr;
}

void FUIWidget::ForEach(const std::function<void(FUIWidget&)>& Visitor)
{
	Visitor(*this);
	for (const std::unique_ptr<FUIWidget>& Child : Children)
	{
		Child->ForEach(Visitor);
	}
}

void FUIWidget::ForEach(const std::function<void(const FUIWidget&)>& Visitor) const
{
	Visitor(*this);
	for (const std::unique_ptr<FUIWidget>& Child : Children)
	{
		static_cast<const FUIWidget&>(*Child).ForEach(Visitor);
	}
}

void FUIWidget::AssignIds()
{
	uint32     NextId = 1;
	const auto Assign = [&NextId](auto& Self, FUIWidget& Widget) -> void {
		Widget.State.Id = NextId++;
		for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
		{
			Child->Parent = &Widget;
			Self(Self, *Child);
		}
	};
	Assign(Assign, *this);
}

bool FUIWidget::IsEnabledInHierarchy() const
{
	for (const FUIWidget* Current = this; Current != nullptr; Current = Current->Parent)
	{
		if (!Current->bEnabled)
		{
			return false;
		}
	}
	return true;
}

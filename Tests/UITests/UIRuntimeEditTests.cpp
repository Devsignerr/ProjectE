#include "Core/Testing/TestFramework.h"
#include "UI/UIAsset.h"
#include "UI/UIInstance.h"
#include "UI/Widget.h"

#include <string>

// FUIInstance::CloneWidget/RemoveWidget 규칙 (UIInstance.h)
E_TEST(UIInstance_CloneWidgetDeepCopiesWithUniqueNamesAndIds)
{
	FUIAsset   Asset;
	FUIWidget* List   = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::VerticalBox));
	List->Name        = "List";
	FUIWidget* Slot   = List->AddChild(FUIWidget::Create(EUIWidgetType::Border));
	Slot->Name        = "Slot";
	FUIWidget* Icon   = Slot->AddChild(FUIWidget::Create(EUIWidgetType::Image));
	Icon->Name        = "Icon";
	FUIWidget* Single = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Button));
	Single->Name      = "Button";
	FUIWidget* Inner  = Single->AddChild(FUIWidget::Create(EUIWidgetType::Image));
	Inner->Name       = "Inner";
	Asset.Root->Name  = "Root";

	FUIInstance Instance(Asset);
	uint32      MaxId = 0;
	Instance.GetRoot().ForEach([&MaxId](const FUIWidget& Widget) { MaxId = FMath::Max(MaxId, Widget.State.Id); });

	std::string Error;
	FUIWidget*  Clone = Instance.CloneWidget("Slot", "Slot_1", Error);
	E_EXPECT_TRUE(Clone != nullptr);
	if (Clone == nullptr)
	{
		return;
	}
	E_EXPECT_TRUE(Clone->Parent == Instance.FindWidget("List"));
	E_EXPECT_EQ(Instance.FindWidget("List")->Children.size(), size_t(2)); // 템플릿 부모 끝에
	E_EXPECT_TRUE(Instance.FindWidget("Slot_1.Icon") != nullptr && Instance.FindWidget("Slot_1.Icon")->Parent == Clone);
	E_EXPECT_TRUE(Instance.FindWidget("Icon")->Parent == Instance.FindWidget("Slot")); // 원본 그대로
	E_EXPECT_TRUE(Clone->State.Id > MaxId && Instance.FindWidget("Slot_1.Icon")->State.Id > MaxId && Clone->State.Id != Instance.FindWidget("Slot_1.Icon")->State.Id);
	E_EXPECT_TRUE(Instance.IsLayoutDirty());

	// 실패: 같은 이름, 없는 템플릿, 루트, 자식을 더 받을 수 없는 부모(버튼 = 1개)
	E_EXPECT_TRUE(Instance.CloneWidget("Slot", "Slot_1", Error) == nullptr);
	E_EXPECT_TRUE(Instance.CloneWidget("Nope", "X", Error) == nullptr);
	E_EXPECT_TRUE(Instance.CloneWidget("Root", "X", Error) == nullptr);
	E_EXPECT_TRUE(Instance.CloneWidget("Inner", "Inner_1", Error) == nullptr);
	E_EXPECT_TRUE(Instance.CloneWidget("Slot", "", Error) == nullptr);

	// 지우기: 번호는 다시 쓰지 않는다
	const uint32 RemovedId = Clone->State.Id;
	E_EXPECT_TRUE(Instance.RemoveWidget("Slot_1"));
	E_EXPECT_TRUE(Instance.FindWidget("Slot_1") == nullptr && Instance.FindWidget("Slot_1.Icon") == nullptr);
	E_EXPECT_FALSE(Instance.RemoveWidget("Slot_1"));
	E_EXPECT_FALSE(Instance.RemoveWidget("Root"));
	FUIWidget* Again = Instance.CloneWidget("Slot", "Slot_1", Error);
	E_EXPECT_TRUE(Again != nullptr && Again->State.Id > RemovedId);
}

#include "UI/UIReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "UI/UIComponent.h"

void RegisterUITypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry::Get().RegisterType<FUIComponent>("UIComponent", "UI")
		.Property(&FUIComponent::Asset, "Asset", "UI 에셋").AssetFilter(".eui")
		.Property(&FUIComponent::ZOrder, "ZOrder", "Z 순서")
		.Property(&FUIComponent::bVisible, "Visible", "보임")
		.Property(&FUIComponent::bReceiveInput, "ReceiveInput", "입력 받기")
		.Property(&FUIComponent::bKeyboardFocus, "KeyboardFocus", "키보드 포커스 (Tab/Enter)")
		.AsComponent();
}

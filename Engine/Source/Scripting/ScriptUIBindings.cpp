#include "Scripting/LuaRuntime.h"

#include "Scene/Scene.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"

#include <stdexcept>

// 게임 UI Lua 바인딩
//   local Health = self.entity:GetWidget("HealthBar")   -- 같은 엔티티의 UIComponent 안 위젯 (없으면 nil)
//   Health.Percent = 0.5; Label.Text = "점수 10"; Menu.Visible = false; Button.Enabled = false
//   function Hud:OnUIClicked_PlayButton() ... end        -- 이벤트: OnUIClicked_/OnUIPressed_/OnUIReleased_/OnUIHoverBegin_/OnUIHoverEnd_<이름>
//   (이름이 Lua 식별자가 아니면 Hud["OnUIClicked_시작"] = function(self) ... end)
// 위젯 값은 접근할 때마다 이름으로 다시 찾는다 (인스턴스가 다시 만들어져도 같은 이름이면 계속 유효)

namespace
{
	struct FScriptWidgetRef
	{
		FEntity     Entity;
		std::string Name;
	};

	const char* GetEventPrefix(EUIEventType Type)
	{
		switch (Type)
		{
		case EUIEventType::Clicked:    return "OnUIClicked_";
		case EUIEventType::Pressed:    return "OnUIPressed_";
		case EUIEventType::Released:   return "OnUIReleased_";
		case EUIEventType::HoverBegin: return "OnUIHoverBegin_";
		case EUIEventType::HoverEnd:   return "OnUIHoverEnd_";
		}
		return "OnUIEvent_";
	}

	// 종류별 "대표 색": 텍스트 = 글자 색, 진행 막대 = 채우기 색, 나머지 = 배경(기본 브러시) 색 (sRGB)
	FVector4& GetMainColor(FUIWidget& Widget)
	{
		switch (Widget.Type)
		{
		case EUIWidgetType::Text:        return Widget.TextColor;
		case EUIWidgetType::ProgressBar: return Widget.FillBrush.Color;
		default:                         return Widget.Brush.Color;
		}
	}
} // namespace

void FLuaRuntime::RegisterUIBindings()
{
	const auto FindInstance = [this](FEntity Entity) -> FUIInstance* {
		if (Scene == nullptr || !Scene->GetRegistry().IsValid(Entity))
		{
			return nullptr;
		}
		FUIComponent* Component = Scene->GetRegistry().TryGet<FUIComponent>(Entity);
		return Component != nullptr ? FUISystem::EnsureInstance(*Component, ContentDirectory) : nullptr;
	};
	const auto RequireWidget = [FindInstance](const FScriptWidgetRef& Ref) -> FUIWidget& {
		FUIInstance* Instance = FindInstance(Ref.Entity);
		FUIWidget*   Widget   = Instance != nullptr ? Instance->FindWidget(Ref.Name) : nullptr;
		if (Widget == nullptr)
		{
			throw std::runtime_error("UI 위젯을 찾을 수 없습니다: " + Ref.Name + " (엔티티의 UIComponent가 사라졌거나 에셋이 바뀌었습니다)");
		}
		return *Widget;
	};

	Lua.new_usertype<FScriptWidgetRef>(
		"UIWidget",
		sol::no_constructor,
		"Name", sol::readonly_property([](const FScriptWidgetRef& Ref) { return Ref.Name; }),
		"Type", sol::readonly_property([RequireWidget](const FScriptWidgetRef& Ref) { return std::string(ToString(RequireWidget(Ref).Type)); }),
		"Text", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).Text; },
		                      [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) { RequireWidget(Ref).Text = Value; }),
		"Percent", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).Percent; },
		                         [RequireWidget](const FScriptWidgetRef& Ref, float Value) { RequireWidget(Ref).Percent = FMath::Clamp(Value, 0.0f, 1.0f); }),
		"Visible",
		sol::property(
			[RequireWidget](const FScriptWidgetRef& Ref) {
				const EUIVisibility Visibility = RequireWidget(Ref).Visibility;
				return Visibility != EUIVisibility::Collapsed && Visibility != EUIVisibility::Hidden;
			},
			[RequireWidget](const FScriptWidgetRef& Ref, bool bVisible) {
				FUIWidget& Widget = RequireWidget(Ref);
				const bool bNow   = Widget.Visibility != EUIVisibility::Collapsed && Widget.Visibility != EUIVisibility::Hidden;
				if (bVisible != bNow)
				{
					Widget.Visibility = bVisible ? FUIWidget::GetDefaultVisibility(Widget.Type) : EUIVisibility::Collapsed;
				}
			}),
		"Visibility", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return std::string(ToString(RequireWidget(Ref).Visibility)); },
		                            [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) {
			                            if (!FromString(Value, RequireWidget(Ref).Visibility))
			                            {
				                            throw std::runtime_error("알 수 없는 Visibility: " + Value +
				                                                     " (Visible/Collapsed/Hidden/HitTestInvisible/SelfHitTestInvisible)");
			                            }
		                            }),
		"Enabled", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).bEnabled; },
		                         [RequireWidget](const FScriptWidgetRef& Ref, bool bValue) { RequireWidget(Ref).bEnabled = bValue; }),
		"Opacity", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).RenderOpacity; },
		                         [RequireWidget](const FScriptWidgetRef& Ref, float Value) { RequireWidget(Ref).RenderOpacity = FMath::Clamp(Value, 0.0f, 1.0f); }),
		"Color", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return GetMainColor(RequireWidget(Ref)); },
		                       [RequireWidget](const FScriptWidgetRef& Ref, const FVector4& Value) { GetMainColor(RequireWidget(Ref)) = Value; }),
		"Texture", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).Brush.Texture; },
		                         [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) { RequireWidget(Ref).Brush.Texture = Value; }),
		"FontSize", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).FontSize; },
		                          [RequireWidget](const FScriptWidgetRef& Ref, float Value) { RequireWidget(Ref).FontSize = FMath::Max(Value, 1.0f); }),
		sol::meta_function::to_string, [](const FScriptWidgetRef& Ref) { return "UIWidget(" + Ref.Name + ")"; });

	sol::usertype<FScriptEntity> EntityType = Lua["Entity"];
	EntityType["GetWidget"] = [this, FindInstance](const FScriptEntity& Entity, const std::string& Name) -> sol::object {
		FUIInstance* Instance = FindInstance(Entity.Entity);
		if (Instance == nullptr || Instance->FindWidget(Name) == nullptr)
		{
			return sol::lua_nil;
		}
		return sol::make_object(Lua, FScriptWidgetRef{ Entity.Entity, Name });
	};
	// 이 엔티티의 UI가 이번 프레임 포인터를 가져갔는지 (게임 쪽 마우스 입력은 이미 막혀 있다 — 표시용)
	EntityType["IsPointerOverUI"] = [this](const FScriptEntity& Entity) {
		const FUIComponent* Component = Scene != nullptr && Scene->GetRegistry().IsValid(Entity.Entity) ? Scene->GetRegistry().TryGet<FUIComponent>(Entity.Entity) : nullptr;
		return Component != nullptr && Component->Runtime.bPointerOver;
	};
}

void FLuaRuntime::DispatchUIEvents()
{
	FRegistry& Registry = Scene->GetRegistry();
	struct FPending
	{
		FEntity  Entity;
		FUIEvent Event;
	};
	std::vector<FPending> Events;
	Registry.View<FUIComponent>().Each([&](FEntity Entity, FUIComponent& Component) {
		for (const FUIEvent& Event : Component.Runtime.Events)
		{
			Events.push_back({ Entity, Event });
		}
	});
	for (const FPending& Pending : Events)
	{
		// 받는 쪽: UI 엔티티의 스크립트, 없으면 가장 가까운 조상의 스크립트
		FScriptInstance* Receiver = nullptr;
		for (FEntity Current = Pending.Entity; Current.IsValid() && Registry.IsValid(Current); Current = Scene->GetParent(Current))
		{
			const auto Found = Instances.find(Current.ToId());
			if (Found != Instances.end())
			{
				Receiver = &Found->second;
				break;
			}
		}
		if (Receiver != nullptr && !Receiver->bFaulted && Receiver->bStarted)
		{
			CallMethod(*Receiver, (std::string(GetEventPrefix(Pending.Event.Type)) + Pending.Event.WidgetName).c_str());
		}
	}
}

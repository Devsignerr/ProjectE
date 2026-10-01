#include "Scripting/LuaRuntime.h"

#include "Scene/Scene.h"
#include "UI/Localization.h"
#include "UI/UIAnimation.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"

#include <stdexcept>

// 게임 UI Lua 바인딩
//   local Health = self.entity:GetWidget("HealthBar")   -- 같은 엔티티의 UIComponent 안 위젯 (없으면 nil)
//   Health.Percent = 0.5; Label.Text = "점수 10"; Menu.Visible = false; Button.Enabled = false
//   function Hud:OnUIClicked_PlayButton() ... end        -- 이벤트: OnUIClicked_/OnUIPressed_/OnUIReleased_/OnUIHoverBegin_/OnUIHoverEnd_<이름>
//                                                         텍스트 상자: OnUITextChanged_/OnUITextCommitted_<이름> (값은 widget.Text)
//   self.entity:PlayUIAnimation("Intro") / StopUIAnimation / IsUIAnimationPlaying, 끝나면 OnUIAnimationFinished_<애니메이션 이름>
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
		case EUIEventType::TextChanged:   return "OnUITextChanged_";
		case EUIEventType::TextCommitted: return "OnUITextCommitted_";
		case EUIEventType::AnimationFinished: return "OnUIAnimationFinished_";
		}
		return "OnUIEvent_";
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
		// 읽기 = 화면에 보이는 글자(문자열 표 키 반영), 쓰기 = 고정 문자열 (키를 떼어 언어를 바꿔도 덮어쓰지 않는다)
		"Text", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return GetDisplayText(RequireWidget(Ref)); },
		                      [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) {
			                      FUIWidget& Widget = RequireWidget(Ref);
			                      Widget.Text       = Value;
			                      if (Widget.Type == EUIWidgetType::Text)
			                      {
				                      Widget.TextKey.clear();
			                      }
		                      }),
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
		"Color", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return FUIAnimMath::GetMainColor(RequireWidget(Ref)); },
		                       [RequireWidget](const FScriptWidgetRef& Ref, const FVector4& Value) { FUIAnimMath::GetMainColor(RequireWidget(Ref)) = Value; }),
		"Texture", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).Brush.Texture; },
		                         [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) { RequireWidget(Ref).Brush.Texture = Value; }),
		"HintText", sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return GetDisplayHintText(RequireWidget(Ref)); },
		                          [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) {
			                          FUIWidget& Widget = RequireWidget(Ref);
			                          Widget.HintText   = Value;
			                          Widget.HintTextKey.clear(); // 고정 문자열 (Text와 같은 규칙)
		                          }),
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
	// UI 애니메이션 (.eui Animations): entity:PlayUIAnimation("Intro"[, 반복(0 = 무한), 속도(음수 = 거꾸로)]) → 있으면 true
	EntityType["PlayUIAnimation"] = [FindInstance](const FScriptEntity& Entity, const std::string& Name, sol::optional<int32> Loops, sol::optional<float> Speed) {
		FUIInstance* Instance = FindInstance(Entity.Entity);
		return Instance != nullptr && Instance->PlayAnimation(Name, Loops.value_or(1), Speed.value_or(1.0f));
	};
	EntityType["StopUIAnimation"] = [FindInstance](const FScriptEntity& Entity, const std::string& Name) {
		if (FUIInstance* Instance = FindInstance(Entity.Entity))
		{
			Instance->StopAnimation(Name);
		}
	};
	EntityType["IsUIAnimationPlaying"] = [FindInstance](const FScriptEntity& Entity, const std::string& Name) {
		const FUIInstance* Instance = FindInstance(Entity.Entity);
		return Instance != nullptr && Instance->IsAnimationPlaying(Name);
	};
	// 이 엔티티의 UI가 이번 프레임 포인터를 가져갔는지 (게임 쪽 마우스 입력은 이미 막혀 있다 — 표시용)
	EntityType["IsPointerOverUI"] = [this](const FScriptEntity& Entity) {
		const FUIComponent* Component = Scene != nullptr && Scene->GetRegistry().IsValid(Entity.Entity) ? Scene->GetRegistry().TryGet<FUIComponent>(Entity.Entity) : nullptr;
		return Component != nullptr && Component->Runtime.bPointerOver;
	};

	// ---- 다국어 (Phase 32): 위젯 키 + Loc 테이블
	//   Label.TextKey = "HUD.Title"            -- 문자열 표 키로 표시 (언어를 바꾸면 바로 바뀐다). "" = 키 해제
	//   Loc.Get("HUD.Score", 120)               -- {0} 위치 인자, Loc.Get("HUD.Hello", { Name = "여우" }) -- {Name} 이름 인자
	//   Loc.SetLanguage("en"[, 저장 = true])    -- 저장하면 다음 실행에도 (<Saved>/Config/Language.json). 표에 없으면 false
	//   Loc.GetLanguage() / Loc.GetLanguages() / Loc.GetLanguageName("en") / Loc.Has("키")
	sol::usertype<FScriptWidgetRef> WidgetType = Lua["UIWidget"];
	WidgetType["TextKey"] = sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).TextKey; },
	                                      [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) { RequireWidget(Ref).TextKey = Value; });
	WidgetType["HintTextKey"] = sol::property([RequireWidget](const FScriptWidgetRef& Ref) { return RequireWidget(Ref).HintTextKey; },
	                                          [RequireWidget](const FScriptWidgetRef& Ref, const std::string& Value) { RequireWidget(Ref).HintTextKey = Value; });

	sol::table LocTable = Lua.create_named_table("Loc");
	LocTable["Get"]     = [this](const std::string& Key, sol::variadic_args Args) {
		FLocFormatArgs           Format;
		sol::protected_function ToString = Lua["tostring"];
		const auto               Stringify = [&ToString](const sol::object& Value) -> std::string {
			if (Value.get_type() == sol::type::string)
			{
				return Value.as<std::string>();
			}
			sol::protected_function_result Result = ToString(Value);
			return Result.valid() ? Result.get<std::string>() : std::string();
		};
		for (const sol::stack_proxy Arg : Args)
		{
			const sol::object Value = Arg;
			if (Value.get_type() == sol::type::table)
			{
				// 표: 문자열 키 = 이름 인자, 1부터 정수 키 = {0}부터 위치 인자
				for (const auto& [TableKey, TableValue] : Value.as<sol::table>())
				{
					if (TableKey.get_type() == sol::type::string)
					{
						Format.Named.emplace_back(TableKey.as<std::string>(), Stringify(TableValue));
					}
					else if (TableKey.get_type() == sol::type::number)
					{
						const int64 Index = TableKey.as<int64>() - 1;
						if (Index >= 0 && Index < 1024)
						{
							if (Format.Positional.size() <= static_cast<size_t>(Index))
							{
								Format.Positional.resize(static_cast<size_t>(Index) + 1);
							}
							Format.Positional[static_cast<size_t>(Index)] = Stringify(TableValue);
						}
					}
				}
			}
			else
			{
				Format.Positional.push_back(Stringify(Value));
			}
		}
		return FLocalization::Get().Get(Key, Format);
	};
	LocTable["SetLanguage"] = [](const std::string& Language, sol::optional<bool> bSave) {
		return FLocalization::Get().SetLanguage(Language, bSave.value_or(true));
	};
	LocTable["GetLanguage"]     = []() { return FLocalization::Get().GetLanguage(); };
	LocTable["GetLanguages"]    = []() { return sol::as_table(FLocalization::Get().GetLanguages()); };
	LocTable["GetLanguageName"] = [](const std::string& Language) { return FLocalization::GetLanguageDisplayName(Language); };
	LocTable["Has"]             = [](const std::string& Key) { return FLocalization::Get().Has(Key); };
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

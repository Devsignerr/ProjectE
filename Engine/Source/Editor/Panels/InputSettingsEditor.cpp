#include "Editor/Panels/InputSettingsEditor.h"

#include "Core/Settings/InputSettings.h"
#include "Editor/EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <initializer_list>

namespace
{
	// std::string 편집 (고정 버퍼)
	bool InputString(const char* Label, std::string& Value, const char* Hint = nullptr)
	{
		char         Buffer[256] = {};
		const size_t Length      = std::min(Value.size(), sizeof(Buffer) - 1);
		std::memcpy(Buffer, Value.data(), Length);
		const bool bChanged = Hint != nullptr ? ImGui::InputTextWithHint(Label, Hint, Buffer, sizeof(Buffer)) : ImGui::InputText(Label, Buffer, sizeof(Buffer));
		if (bChanged)
		{
			Value = Buffer;
		}
		return bChanged;
	}

	bool SourceCombo(const char* Label, FInputSource& Source)
	{
		bool              bChanged = false;
		const std::string Current  = InputNames::ToString(Source);
		if (ImGui::BeginCombo(Label, Current.c_str(), ImGuiComboFlags_HeightLarge))
		{
			static char Filter[64] = {};
			if (ImGui::IsWindowAppearing())
			{
				Filter[0] = '\0';
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##Filter", ICON_FA_MAGNIFYING_GLASS " 찾기", Filter, sizeof(Filter));
			for (const std::string& Name : InputNames::GetAllSourceNames())
			{
				if (Filter[0] != '\0' && Name.find(Filter) == std::string::npos)
				{
					continue;
				}
				if (ImGui::Selectable(Name.c_str(), Name == Current))
				{
					bChanged = InputNames::TryParseSource(Name, Source);
				}
			}
			ImGui::EndCombo();
		}
		return bChanged;
	}

	template <typename TEnum>
	bool EnumCombo(const char* Label, TEnum& Value, std::initializer_list<TEnum> Values)
	{
		bool bChanged = false;
		if (ImGui::BeginCombo(Label, InputNames::ToString(Value)))
		{
			for (const TEnum Option : Values)
			{
				if (ImGui::Selectable(InputNames::ToString(Option), Option == Value))
				{
					Value    = Option;
					bChanged = true;
				}
			}
			ImGui::EndCombo();
		}
		return bChanged;
	}

	bool DrawModifier(FInputModifier& Modifier)
	{
		bool bChanged = false;
		ImGui::SetNextItemWidth(130.0f);
		bChanged |= EnumCombo("##Type", Modifier.Type, { EInputModifierType::Negate, EInputModifierType::Swizzle, EInputModifierType::DeadZone,
		                                                  EInputModifierType::Scale, EInputModifierType::ScaleByDeltaTime });
		ImGui::SameLine();
		switch (Modifier.Type)
		{
		case EInputModifierType::Negate:
			bChanged |= ImGui::Checkbox("X##Neg", &Modifier.bX);
			ImGui::SameLine();
			bChanged |= ImGui::Checkbox("Y##Neg", &Modifier.bY);
			break;
		case EInputModifierType::DeadZone:
			ImGui::SetNextItemWidth(140.0f);
			bChanged |= ImGui::DragFloatRange2("##Range", &Modifier.Lower, &Modifier.Upper, 0.01f, 0.0f, 1.0f, "하한 %.2f", "상한 %.2f");
			ImGui::SameLine();
			bChanged |= ImGui::Checkbox("원형", &Modifier.bRadial);
			ImGui::SetItemTooltip("켜면 길이 기준(스틱), 끄면 축별");
			break;
		case EInputModifierType::Scale:
			ImGui::SetNextItemWidth(160.0f);
			bChanged |= ImGui::DragFloat2("##Scale", &Modifier.Scale.X, 0.01f);
			break;
		case EInputModifierType::Swizzle:
			ImGui::TextDisabled("X ↔ Y");
			break;
		case EInputModifierType::ScaleByDeltaTime:
			ImGui::TextDisabled("× 프레임 시간 (초당 값 → 프레임당)");
			break;
		}
		return bChanged;
	}

	// 반환: 바뀜. bRemove = 이 액션 삭제 요청
	bool DrawAction(FInputAction& Action, bool& bRemove)
	{
		bool bChanged = false;
		ImGui::SetNextItemWidth(160.0f);
		bChanged |= InputString("이름", Action.Name);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(100.0f);
		bChanged |= EnumCombo("종류", Action.Type, { EInputActionType::Button, EInputActionType::Axis1D, EInputActionType::Axis2D });
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		bChanged |= ImGui::DragFloat("작동 문턱", &Action.ActuationThreshold, 0.01f, 0.0f, 10.0f, "%.2f");
		ImGui::SetItemTooltip("값의 크기(2D는 길이)가 이 이상이면 켜짐 — 눌림/떼어짐 판정");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Danger);
		if (ImGui::SmallButton(ICON_FA_TRASH "##RemoveAction"))
		{
			bRemove = true;
		}
		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("액션 삭제");
		ImGui::SetNextItemWidth(-FLT_MIN);
		bChanged |= InputString("##Description", Action.Description, "설명");

		int32 RemoveBinding = -1;
		for (size_t Index = 0; Index < Action.Bindings.size(); ++Index)
		{
			FInputBinding& Binding = Action.Bindings[Index];
			ImGui::PushID(static_cast<int>(Index));
			ImGui::Indent(12.0f);
			const char* Icon = Binding.Source.Type == EInputSourceType::GamepadButton || Binding.Source.Type == EInputSourceType::GamepadAxis
			                       ? ICON_FA_GAMEPAD
			                       : (Binding.Source.Type == EInputSourceType::Key ? ICON_FA_KEYBOARD : ICON_FA_COMPUTER_MOUSE);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Icon);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(200.0f);
			bChanged |= SourceCombo("##Source", Binding.Source);
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_PLUS " 수정자"))
			{
				Binding.Modifiers.push_back(FInputModifier::MakeNegate());
				bChanged = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_XMARK "##RemoveBinding"))
			{
				RemoveBinding = static_cast<int32>(Index);
			}
			ImGui::SetItemTooltip("바인딩 삭제");

			int32 RemoveModifier = -1;
			for (size_t ModIndex = 0; ModIndex < Binding.Modifiers.size(); ++ModIndex)
			{
				ImGui::PushID(static_cast<int>(ModIndex));
				ImGui::Indent(24.0f);
				ImGui::TextDisabled("%zu.", ModIndex + 1);
				ImGui::SameLine();
				bChanged |= DrawModifier(Binding.Modifiers[ModIndex]);
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_FA_XMARK "##RemoveModifier"))
				{
					RemoveModifier = static_cast<int32>(ModIndex);
				}
				ImGui::Unindent(24.0f);
				ImGui::PopID();
			}
			if (RemoveModifier >= 0)
			{
				Binding.Modifiers.erase(Binding.Modifiers.begin() + RemoveModifier);
				bChanged = true;
			}
			ImGui::Unindent(12.0f);
			ImGui::PopID();
		}
		if (RemoveBinding >= 0)
		{
			Action.Bindings.erase(Action.Bindings.begin() + RemoveBinding);
			bChanged = true;
		}
		ImGui::Indent(12.0f);
		if (ImGui::SmallButton(ICON_FA_PLUS " 바인딩 추가"))
		{
			Action.Bindings.push_back({ FInputSource::Key(EKey::Space), {} });
			bChanged = true;
		}
		ImGui::Unindent(12.0f);
		return bChanged;
	}
} // namespace

bool FInputSettingsEditor::Draw(FInputSettings& Settings)
{
	FInputMapping Mapping  = Settings.GetProjectMapping(); // 사본을 고쳐 바뀌면 반영
	bool          bChanged = false;

	ImGui::TextDisabled("액션 값 = 바인딩 결과의 합. 수정자는 위에서부터 차례로 적용 (예: W = 축 바꿈 → (0,1), S = 축 바꿈 + 부정 → (0,-1))");
	ImGui::TextDisabled("게임 코드: Lua Input.GetAction(\"Move\") / IsActionPressed / WasActionPressed, C++ FInput::GetActionValue");
	if (Settings.IsUserFileEnabled())
	{
		ImGui::TextDisabled("플레이어 재지정 파일이 프로젝트 바인딩 위에 덮입니다 (Lua Input.Rebind / ResetBindings)");
	}

	int32 RemoveAction = -1;
	for (size_t Index = 0; Index < Mapping.Actions.size(); ++Index)
	{
		FInputAction& Action = Mapping.Actions[Index];
		ImGui::PushID(static_cast<int>(Index));
		const std::string Header = std::format("{} {}  ({}, 바인딩 {}개)###Action", Action.Type == EInputActionType::Button ? ICON_FA_TOGGLE_ON : ICON_FA_UP_DOWN_LEFT_RIGHT,
		                                       Action.Name, InputNames::ToString(Action.Type), Action.Bindings.size());
		if (ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool bRemove = false;
			bChanged |= DrawAction(Action, bRemove);
			if (bRemove)
			{
				RemoveAction = static_cast<int32>(Index);
			}
		}
		ImGui::PopID();
	}
	if (RemoveAction >= 0)
	{
		Mapping.Actions.erase(Mapping.Actions.begin() + RemoveAction);
		bChanged = true;
	}
	ImGui::Spacing();
	if (ImGui::Button(ICON_FA_PLUS " 액션 추가"))
	{
		FInputAction Action;
		for (int32 Number = 1;; ++Number)
		{
			Action.Name = Number == 1 ? std::string("NewAction") : std::format("NewAction{}", Number);
			if (Mapping.Find(Action.Name) == nullptr)
			{
				break;
			}
		}
		Mapping.Actions.push_back(std::move(Action));
		bChanged = true;
	}

	// 이름 중복/빈 이름은 저장 형식에서 건너뛰므로 경고
	for (const FInputAction& Action : Mapping.Actions)
	{
		const auto Count = std::count_if(Mapping.Actions.begin(), Mapping.Actions.end(), [&Action](const FInputAction& Other) { return Other.Name == Action.Name; });
		if (Action.Name.empty() || Count > 1)
		{
			ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 액션 이름이 비었거나 겹칩니다: '%s' (저장 시 건너뜀)", Action.Name.c_str());
			break;
		}
	}

	if (bChanged)
	{
		Settings.SetProjectMapping(std::move(Mapping));
	}
	return bChanged;
}

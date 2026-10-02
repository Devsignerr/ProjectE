#include "Editor/ConsoleVariableWidgets.h"

#include "Core/Console/Console.h"

#include <imgui.h>

namespace
{
	// 툴팁: 변수 이름 + 도움말 (콘솔에서 같은 값을 바꿀 수 있음을 알린다)
	void ItemTooltip(const FConsoleVariable& Variable)
	{
		ImGui::SetItemTooltip("%s — %s", Variable.GetName().c_str(), Variable.GetHelp().c_str());
	}

	bool Missing(const char* Label)
	{
		ImGui::BeginDisabled();
		ImGui::TextUnformatted(Label);
		ImGui::EndDisabled();
		return false;
	}
} // namespace

namespace ConsoleVariableWidgets
{
	bool Checkbox(const char* Label, const char* VariableName)
	{
		FConsoleVariable* Variable = FConsoleManager::Get().FindVariable(VariableName);
		if (Variable == nullptr)
		{
			return Missing(Label);
		}
		bool       bValue   = Variable->GetBool();
		const bool bChanged = ImGui::Checkbox(Label, &bValue);
		ItemTooltip(*Variable);
		if (bChanged)
		{
			Variable->SetBool(bValue);
		}
		return bChanged;
	}

	bool SliderFloat(const char* Label, const char* VariableName, float Min, float Max, const char* Format)
	{
		FConsoleVariable* Variable = FConsoleManager::Get().FindVariable(VariableName);
		if (Variable == nullptr)
		{
			return Missing(Label);
		}
		float      Value    = Variable->GetFloat();
		const bool bChanged = ImGui::SliderFloat(Label, &Value, Min, Max, Format);
		ItemTooltip(*Variable);
		if (bChanged)
		{
			Variable->SetFloat(Value);
		}
		return bChanged;
	}

	bool Combo(const char* Label, const char* VariableName)
	{
		FConsoleVariable* Variable = FConsoleManager::Get().FindVariable(VariableName);
		if (Variable == nullptr)
		{
			return Missing(Label);
		}
		const std::vector<std::string>& Names    = Variable->GetDesc().ValueNames;
		const int32                     Current  = Variable->GetInt();
		bool                            bChanged = false;
		const char*                     Preview  = Current >= 0 && static_cast<size_t>(Current) < Names.size() ? Names[static_cast<size_t>(Current)].c_str() : "?";
		if (ImGui::BeginCombo(Label, Preview))
		{
			for (size_t Index = 0; Index < Names.size(); ++Index)
			{
				if (ImGui::Selectable(Names[Index].c_str(), static_cast<int32>(Index) == Current))
				{
					Variable->SetInt(static_cast<int32>(Index));
					bChanged = true;
				}
			}
			ImGui::EndCombo();
		}
		ItemTooltip(*Variable);
		return bChanged;
	}

	bool GetBool(const char* VariableName, bool Fallback)
	{
		const FConsoleVariable* Variable = FConsoleManager::Get().FindVariable(VariableName);
		return Variable != nullptr ? Variable->GetBool() : Fallback;
	}
} // namespace ConsoleVariableWidgets

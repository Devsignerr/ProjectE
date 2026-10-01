#include "Editor/AssetEditors/StringTableEditor.h"

#include "Core/Log.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	// std::string 편집 (버퍼 크기 자동). 반환: 바뀜
	int InputTextResize(ImGuiInputTextCallbackData* Data)
	{
		if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
		{
			auto* Text = static_cast<std::string*>(Data->UserData);
			Text->resize(static_cast<size_t>(Data->BufTextLen));
			Data->Buf = Text->data();
		}
		return 0;
	}

	bool InputString(const char* Label, std::string& Text, float Width)
	{
		ImGui::SetNextItemWidth(Width);
		return ImGui::InputText(Label, Text.data(), Text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, InputTextResize, &Text);
	}

	bool ContainsInsensitive(std::string_view Text, std::string_view Needle)
	{
		if (Needle.empty())
		{
			return true;
		}
		const auto It = std::search(Text.begin(), Text.end(), Needle.begin(), Needle.end(), [](char A, char B) {
			return std::tolower(static_cast<unsigned char>(A)) == std::tolower(static_cast<unsigned char>(B));
		});
		return It != Text.end();
	}
} // namespace

bool FStringTableEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	FStringTable Loaded;
	if (!Loaded.LoadFromFile(Path))
	{
		if (Env.Editor && Env.Editor->Notify)
		{
			Env.Editor->Notify("문자열 표를 읽지 못했습니다: " + GetDisplayName(), true);
		}
		return false;
	}
	Table = std::move(Loaded);
	if (Table.Strings.find(SelectedKey) == Table.Strings.end())
	{
		SelectedKey.clear();
	}
	return true;
}

bool FStringTableEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	if (!Table.SaveToFile(Path))
	{
		return false;
	}
	FLocalization::Get().Reload(); // 열린 UI가 바로 새 문자열을 쓴다
	return true;
}

std::string FStringTableEditor::CaptureState() const
{
	return Table.ToJsonString();
}

void FStringTableEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	FStringTable Restored;
	if (Restored.FromJsonString(State))
	{
		Table = std::move(Restored);
		if (Table.Strings.find(SelectedKey) == Table.Strings.end())
		{
			SelectedKey.clear();
		}
	}
}

bool FStringTableEditor::IsMissingTranslation(const std::string& Key) const
{
	const auto It = Table.Strings.find(Key);
	if (It == Table.Strings.end())
	{
		return true;
	}
	for (const std::string& Language : Table.Languages)
	{
		const auto Value = It->second.find(Language);
		if (Value == It->second.end() || Value->second.empty())
		{
			return true;
		}
	}
	return false;
}

void FStringTableEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	(void)Env;
	// 도구 줄: 검색 / 번역 빠진 것만 / 키 추가
	ImGui::SetNextItemWidth(220.0f);
	ImGui::InputTextWithHint("##Filter", ICON_FA_MAGNIFYING_GLASS " 키/문자열 검색", Filter, sizeof(Filter));
	ImGui::SameLine();
	ImGui::Checkbox("번역 빠진 것만", &bOnlyMissing);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200.0f);
	const bool bEnter = ImGui::InputTextWithHint("##NewKey", "새 키 (예: Menu.Play)", NewKey, sizeof(NewKey), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	const std::string NewKeyText = NewKey;
	const bool        bExists    = Table.Strings.find(NewKeyText) != Table.Strings.end();
	ImGui::BeginDisabled(NewKeyText.empty() || bExists);
	if ((ImGui::Button(ICON_FA_PLUS " 키 추가") || bEnter) && !NewKeyText.empty() && !bExists)
	{
		Table.Strings[NewKeyText];
		SelectedKey = NewKeyText;
		NewKey[0]   = '\0';
		MarkEdited("키 추가");
	}
	ImGui::EndDisabled();
	if (bExists)
	{
		ImGui::SameLine();
		ImGui::TextColored(FEditorTheme::Warning, "이미 있는 키");
	}

	const int32 Columns = 1 + static_cast<int32>(Table.Languages.size());
	if (Columns > 63)
	{
		ImGui::TextColored(FEditorTheme::Danger, "언어가 너무 많습니다");
		return;
	}
	const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
	                              ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("##Strings", Columns, Flags, ImGui::GetContentRegionAvail()))
	{
		return;
	}
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("키", ImGuiTableColumnFlags_WidthStretch, 0.8f);
	for (const std::string& Language : Table.Languages)
	{
		const std::string Header = std::format("{} ({})", FLocalization::GetLanguageDisplayName(Language), Language);
		ImGui::TableSetupColumn(Header.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
	}
	ImGui::TableHeadersRow();

	for (auto& [Key, Values] : Table.Strings)
	{
		if (bOnlyMissing && !IsMissingTranslation(Key))
		{
			continue;
		}
		if (Filter[0] != '\0')
		{
			bool bMatch = ContainsInsensitive(Key, Filter);
			for (const auto& [Language, Value] : Values)
			{
				bMatch = bMatch || ContainsInsensitive(Value, Filter);
			}
			if (!bMatch)
			{
				continue;
			}
		}
		ImGui::PushID(Key.c_str());
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		if (ImGui::Selectable(Key.c_str(), SelectedKey == Key, ImGuiSelectableFlags_AllowOverlap))
		{
			SelectedKey = Key;
			std::snprintf(RenameBuffer, sizeof(RenameBuffer), "%s", Key.c_str());
		}
		for (const std::string& Language : Table.Languages)
		{
			ImGui::TableNextColumn();
			ImGui::PushID(Language.c_str());
			std::string& Value  = Values[Language];
			const bool   bEmpty = Value.empty();
			if (bEmpty)
			{
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.25f, 0.10f, 0.45f)); // 번역 빠짐
			}
			if (InputString("##Value", Value, -FLT_MIN))
			{
				MarkEdited("문자열 편집");
			}
			if (bEmpty)
			{
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemActivated())
			{
				SelectedKey = Key;
				std::snprintf(RenameBuffer, sizeof(RenameBuffer), "%s", Key.c_str());
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void FStringTableEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	(void)Env;
	if (ImGui::CollapsingHeader(ICON_FA_LANGUAGE " 언어", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t Index = 0; Index < Table.Languages.size(); ++Index)
		{
			const std::string& Language = Table.Languages[Index];
			ImGui::PushID(static_cast<int32>(Index));
			ImGui::BulletText("%s (%s)", FLocalization::GetLanguageDisplayName(Language).c_str(), Language.c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(Index == 0);
			if (ImGui::SmallButton(ICON_FA_ARROW_UP))
			{
				std::swap(Table.Languages[Index], Table.Languages[Index - 1]);
				MarkEdited("언어 순서");
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_TRASH))
			{
				PendingDeleteLanguage = Language;
			}
			ImGui::SetItemTooltip("이 언어와 그 번역을 모두 지웁니다");
			ImGui::PopID();
		}
		ImGui::SetNextItemWidth(100.0f);
		const bool bEnter = ImGui::InputTextWithHint("##NewLanguage", "ja, zh-cn ...", NewLanguage, sizeof(NewLanguage), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if ((ImGui::Button(ICON_FA_PLUS " 언어 추가") || bEnter) && Table.AddLanguage(NewLanguage))
		{
			NewLanguage[0] = '\0';
			MarkEdited("언어 추가");
		}
		FAssetEditorWidgets::Hint("언어 코드는 소문자 (ko, en, ja, zh-cn). 첫 언어가 표의 기준이며, 현재 언어에 없는 문자열은 프로젝트 기본 언어로 보입니다.");
	}

	if (ImGui::CollapsingHeader(ICON_FA_KEY " 선택한 키", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const auto It = Table.Strings.find(SelectedKey);
		if (It == Table.Strings.end())
		{
			ImGui::TextDisabled("표에서 키를 고르세요");
		}
		else
		{
			ImGui::InputText("이름", RenameBuffer, sizeof(RenameBuffer));
			const std::string NewName = RenameBuffer;
			const bool        bTaken  = NewName != SelectedKey && Table.Strings.find(NewName) != Table.Strings.end();
			ImGui::BeginDisabled(NewName.empty() || NewName == SelectedKey || bTaken);
			if (ImGui::Button(ICON_FA_PEN " 이름 바꾸기"))
			{
				auto Values = std::move(It->second);
				Table.Strings.erase(It);
				Table.Strings[NewName] = std::move(Values);
				SelectedKey            = NewName;
				MarkEdited("키 이름 바꾸기");
			}
			ImGui::EndDisabled();
			if (bTaken)
			{
				ImGui::SameLine();
				ImGui::TextColored(FEditorTheme::Warning, "이미 있는 키");
			}
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_TRASH " 삭제"))
			{
				PendingDeleteKey = SelectedKey;
			}
			FAssetEditorWidgets::Hint("이름을 바꾸면 이 키를 쓰는 UI/스크립트도 직접 고쳐야 합니다. 문자열 안 {0}, {Name}은 형식 인자 자리입니다 ({{ }}는 중괄호).");
		}
	}
	if (ImGui::CollapsingHeader(ICON_FA_CIRCLE_INFO " 요약", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("키 %d개", static_cast<int32>(Table.Strings.size()));
		for (const std::string& Language : Table.Languages)
		{
			int32 Missing = 0;
			for (const auto& [Key, Values] : Table.Strings)
			{
				const auto Value = Values.find(Language);
				Missing += Value == Values.end() || Value->second.empty() ? 1 : 0;
			}
			if (Missing > 0)
			{
				ImGui::TextColored(FEditorTheme::Warning, "%s: 번역 %d개 빠짐", Language.c_str(), Missing);
			}
			else
			{
				ImGui::TextColored(FEditorTheme::Success, "%s: 모두 번역됨", Language.c_str());
			}
		}
	}

	// 순회가 끝난 뒤 삭제
	if (!PendingDeleteKey.empty())
	{
		Table.Strings.erase(PendingDeleteKey);
		SelectedKey.clear();
		PendingDeleteKey.clear();
		MarkEdited("키 삭제");
	}
	if (!PendingDeleteLanguage.empty())
	{
		std::erase(Table.Languages, PendingDeleteLanguage);
		for (auto& [Key, Values] : Table.Strings)
		{
			Values.erase(PendingDeleteLanguage);
		}
		PendingDeleteLanguage.clear();
		MarkEdited("언어 삭제");
	}
}

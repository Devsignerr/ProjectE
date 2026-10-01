#include "Editor/Panels/SettingsWindow.h"

#include "Core/Settings/SettingsRegistry.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/PropertyWidgets.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace
{
	// 대소문자 무시 부분 문자열 (한글은 그대로 비교)
	bool ContainsText(const std::string& Haystack, const std::string& Needle)
	{
		if (Needle.empty())
		{
			return true;
		}
		const auto It = std::search(Haystack.begin(), Haystack.end(), Needle.begin(), Needle.end(), [](char A, char B)
		{
			return std::tolower(static_cast<unsigned char>(A)) == std::tolower(static_cast<unsigned char>(B));
		});
		return It != Haystack.end();
	}
} // namespace

void FSettingsWindow::Open(const std::string& SectionId)
{
	bOpen = true;
	if (!SectionId.empty())
	{
		SelectedId = SectionId;
	}
	bFocusRequested = true; // 다음 Draw에서 앞으로 (ImGui 프레임 밖에서도 부를 수 있게)
}

bool FSettingsWindow::IsListed(const FSettingsSection& Section) const
{
	if (Section.bHidden)
	{
		return false;
	}
	return Kind == EKind::ProjectSettings ? Section.Scope == ESettingsScope::Project : Section.Scope == ESettingsScope::EditorUser;
}

void FSettingsWindow::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		SavePendingSections();
		return;
	}
	const bool        bProject = Kind == EKind::ProjectSettings;
	const std::string Title    = bProject ? FEditorTheme::PanelTitle(ICON_FA_SLIDERS, "프로젝트 설정", "ProjectSettings")
	                                      : FEditorTheme::PanelTitle(ICON_FA_GEAR, "에디터 환경설정", "EditorPreferences");
	ImGui::SetNextWindowSize(ImVec2(920.0f, 620.0f), ImGuiCond_FirstUseEver);
	if (bFocusRequested)
	{
		ImGui::SetNextWindowFocus();
		bFocusRequested = false;
	}
	if (!ImGui::Begin(Title.c_str(), &bOpen))
	{
		ImGui::End();
		SavePendingSections();
		return;
	}

	// 선택 섹션이 없거나 목록에 없으면 첫 섹션
	FSettingsRegistry& Registry = FSettingsRegistry::Get();
	FSettingsSection*  Selected = Registry.Find(SelectedId);
	if (Selected == nullptr || !IsListed(*Selected))
	{
		Selected = nullptr;
		for (const std::unique_ptr<FSettingsSection>& Section : Registry.GetSections())
		{
			if (IsListed(*Section))
			{
				Selected   = Section.get();
				SelectedId = Section->Id;
				break;
			}
		}
	}

	// 왼쪽: 검색 + 섹션 목록
	ImGui::BeginChild("Sections", ImVec2(220.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " 검색", SearchText, sizeof(SearchText));
	ImGui::Separator();
	DrawSectionList();
	ImGui::EndChild();
	ImGui::SameLine();

	// 오른쪽: 검색 중이면 모든 섹션의 일치 항목, 아니면 선택 섹션
	ImGui::BeginChild("Properties", ImVec2(0.0f, 0.0f));
	const std::string Filter = SearchText;
	if (!Filter.empty())
	{
		int Found = 0;
		for (const std::unique_ptr<FSettingsSection>& Section : Registry.GetSections())
		{
			if (IsListed(*Section))
			{
				Found += DrawSectionProperties(Context, *Section, Filter);
			}
		}
		if (Found == 0)
		{
			ImGui::TextDisabled("'%s'에 해당하는 설정이 없습니다", SearchText);
		}
	}
	else if (Selected != nullptr)
	{
		ImGui::PushFont(FEditorTheme::GetBoldFont());
		ImGui::TextUnformatted(Selected->DisplayName.c_str());
		ImGui::PopFont();
		if (!Selected->Description.empty())
		{
			ImGui::TextDisabled("%s", Selected->Description.c_str());
		}
		if (Selected->bRequiresRestart)
		{
			ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 바꾼 값은 다시 시작한 뒤 적용됩니다");
		}
		const std::string FilePath = FStringConv::ToUtf8(Selected->GetFilePath().wstring());
		ImGui::TextDisabled("%s %s", bProject ? "저장 위치 (저장소에 커밋):" : "저장 위치 (개인):", FilePath.c_str());
		if (ImGui::Button(ICON_FA_ROTATE_LEFT " 기본값으로"))
		{
			Selected->ResetToDefaults();
			if (Selected->OnChanged)
			{
				Selected->OnChanged();
			}
			PendingSave.insert(Selected->Id);
		}
		ImGui::SetItemTooltip("이 섹션의 모든 값을 엔진 기본값으로 되돌리고 저장합니다");
		ImGui::Separator();
		DrawSectionProperties(Context, *Selected, std::string());
	}
	ImGui::EndChild();
	ImGui::End();

	// 드래그/입력이 끝난 뒤 저장 (매 프레임 쓰지 않게)
	if (!ImGui::IsAnyItemActive())
	{
		SavePendingSections();
	}
}

void FSettingsWindow::DrawSectionList()
{
	// 카테고리 순서 = 처음 등록된 순서
	std::vector<std::string> Categories;
	for (const std::unique_ptr<FSettingsSection>& Section : FSettingsRegistry::Get().GetSections())
	{
		if (IsListed(*Section) && std::find(Categories.begin(), Categories.end(), Section->Category) == Categories.end())
		{
			Categories.push_back(Section->Category);
		}
	}
	for (const std::string& Category : Categories)
	{
		ImGui::TextDisabled("%s", Category.c_str());
		for (const std::unique_ptr<FSettingsSection>& Section : FSettingsRegistry::Get().GetSections())
		{
			if (!IsListed(*Section) || Section->Category != Category)
			{
				continue;
			}
			ImGui::Indent(8.0f);
			if (ImGui::Selectable(Section->DisplayName.c_str(), Section->Id == SelectedId && SearchText[0] == '\0'))
			{
				SelectedId    = Section->Id;
				SearchText[0] = '\0';
			}
			ImGui::Unindent(8.0f);
		}
		ImGui::Spacing();
	}
}

int FSettingsWindow::DrawSectionProperties(FEditorContext& Context, FSettingsSection& Section, const std::string& Filter)
{
	std::vector<const FPropertyInfo*> Properties;
	for (const FPropertyInfo& Property : Section.Type->Properties)
	{
		if (!Property.HasFlag(PF_Hidden) && FPropertyWidgets::IsValueType(Property.Type) &&
		    (ContainsText(Property.DisplayName, Filter) || ContainsText(Property.Name, Filter) || ContainsText(Section.DisplayName, Filter)))
		{
			Properties.push_back(&Property);
		}
	}
	if (Properties.empty())
	{
		return 0;
	}
	if (!Filter.empty())
	{
		ImGui::SeparatorText(std::format("{} › {}", Section.Category, Section.DisplayName).c_str());
	}

	ImGui::PushID(Section.Id.c_str());
	if (ImGui::BeginTable("Props", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthFixed, 230.0f);
		ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch);
		for (const FPropertyInfo* Property : Properties)
		{
			ImGui::PushID(Property->Name.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Property->DisplayName.c_str());
			if (!Property->Tooltip.empty())
			{
				ImGui::SetItemTooltip("%s", Property->Tooltip.c_str());
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::BeginDisabled(Property->HasFlag(PF_ReadOnly));
			bool bChanged = false;
			if (Property->Type == EPropertyType::String && !Property->AssetFilter.empty())
			{
				bool bRejected = false;
				bChanged       = FPropertyWidgets::DrawAssetPath(*Property, Section.Object, "##Value", &bRejected);
				if (bRejected && Context.Notify)
				{
					Context.Notify(std::format("{} 칸에는 {} 파일만 놓을 수 있습니다", Property->DisplayName, Property->AssetFilter), true);
				}
			}
			else
			{
				bChanged = FPropertyWidgets::DrawValue(*Property, Section.Object, "##Value");
			}
			ImGui::EndDisabled();
			if (bChanged)
			{
				if (Section.OnChanged)
				{
					Section.OnChanged();
				}
				PendingSave.insert(Section.Id);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::PopID();
	return static_cast<int>(Properties.size());
}

void FSettingsWindow::SavePendingSections()
{
	for (const std::string& Id : PendingSave)
	{
		if (const FSettingsSection* Section = FSettingsRegistry::Get().Find(Id))
		{
			Section->Save();
		}
	}
	PendingSave.clear();
}

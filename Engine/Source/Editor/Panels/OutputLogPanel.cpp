#include "Editor/Panels/OutputLogPanel.h"

#include "Editor/EditorTheme.h"

#include <imgui.h>

void FOutputLogPanel::Draw(FEditorContext&)
{
	if (!bOpen)
	{
		return;
	}

	auto NewMessages = FLog::ReadHistory(LastSequence);
	const bool bHasNewMessages = !NewMessages.empty();
	for (FLogMessage& Message : NewMessages)
	{
		LastSequence = Message.Sequence;
		Messages.push_back(std::move(Message));
	}
	while (Messages.size() > FLog::MaxHistoryMessages)
	{
		Messages.pop_front();
	}

	ImGui::SetNextWindowSize(ImVec2(900.0f, 260.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_TERMINAL, "출력 로그", "OutputLog").c_str(), &bOpen))
	{
		ImGui::SetNextItemWidth(280.0f);
		ImGui::InputTextWithHint("##LogSearch", "로그 검색", Search, sizeof(Search));
		ImGui::SameLine();
		if (ImGui::Button("지우기"))
		{
			Messages.clear();
		}
		ImGui::SameLine();
		ImGui::Checkbox("자동 스크롤", &bAutoScroll);
		ImGui::SameLine();
		ImGui::TextDisabled("최근 %zu개", Messages.size());
		ImGui::Separator();

		if (ImGui::BeginChild("LogLines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
		{
			const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
			for (const FLogMessage& Message : Messages)
			{
				if (Search[0] != '\0' && Message.Text.find(Search) == std::string::npos)
				{
					continue;
				}
				const ImVec4 Color = Message.Verbosity <= ELogVerbosity::Error ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) :
					Message.Verbosity == ELogVerbosity::Warning ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
				ImGui::PushStyleColor(ImGuiCol_Text, Color);
				const size_t Length = Message.Text.size();
				const size_t DisplayLength = Length > 0 && Message.Text.back() == '\n' ? Length - 1 : Length;
				ImGui::TextUnformatted(Message.Text.data(), Message.Text.data() + DisplayLength);
				ImGui::PopStyleColor();
			}
			if (bAutoScroll && bWasAtBottom && bHasNewMessages)
			{
				ImGui::SetScrollHereY(1.0f);
			}
		}
		ImGui::EndChild();
	}
	ImGui::End();
}

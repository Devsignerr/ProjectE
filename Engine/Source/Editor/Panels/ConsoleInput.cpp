#include "Editor/Panels/ConsoleInput.h"

#include "Core/Console/Console.h"
#include "Editor/EditorTheme.h"

#include <imgui.h>
#include <imgui_internal.h> // BringWindowToDisplayFront (후보 팝업을 도킹 창 위로)

#include <algorithm>
#include <cstring>

namespace
{
	constexpr size_t MaxCompletionShown = 12;
} // namespace

void FConsoleInput::SetText(std::string_view Text, bool bFocus)
{
	const size_t Length = std::min(Text.size(), sizeof(Buffer) - 1);
	std::memcpy(Buffer, Text.data(), Length);
	Buffer[Length] = '\0';
	HistoryIndex   = -1;
	bRequestFocus  = bRequestFocus || bFocus;
}

void FConsoleInput::RefreshCompletions()
{
	if (CompletionSource == Buffer)
	{
		return;
	}
	CompletionSource = Buffer;
	CompletionTexts.clear();
	CompletionNames.clear();
	CompletionDetails.clear();
	CompletionIsCommand.clear();
	SelectedCompletion = -1;
	if (Buffer[0] == '\0' || HistoryIndex >= 0)
	{
		return; // 기록에서 꺼낸 줄은 후보를 띄우지 않는다
	}
	for (FConsoleCompletion& Completion : FConsoleManager::Get().GetCompletions(Buffer, MaxCompletionShown))
	{
		if (Completion.Text == Buffer)
		{
			continue;
		}
		CompletionTexts.push_back(std::move(Completion.Text));
		CompletionNames.push_back(std::move(Completion.Name));
		CompletionDetails.push_back(std::move(Completion.Detail));
		CompletionIsCommand.push_back(Completion.bIsCommand);
	}
}

void FConsoleInput::ReplaceText(ImGuiInputTextCallbackData* Data, const std::string& Text)
{
	Data->DeleteChars(0, Data->BufTextLen);
	Data->InsertChars(0, Text.c_str());
	Data->CursorPos = Data->SelectionStart = Data->SelectionEnd = Data->BufTextLen;
}

int FConsoleInput::HandleCallback(ImGuiInputTextCallbackData* Data)
{
	FConsoleInput& Self = *static_cast<FConsoleInput*>(Data->UserData);
	switch (Data->EventFlag)
	{
	case ImGuiInputTextFlags_CallbackAlways:
		if (Self.bHasPendingText)
		{
			// 포커스를 다시 얻은 첫 프레임: 전체 선택 대신 커서를 끝으로
			Self.bHasPendingText = false;
			Data->CursorPos = Data->SelectionStart = Data->SelectionEnd = Data->BufTextLen;
		}
		break;
	case ImGuiInputTextFlags_CallbackCompletion:
	{
		std::string Text;
		if (Self.SelectedCompletion >= 0 && Self.SelectedCompletion < static_cast<int32>(Self.CompletionTexts.size()))
		{
			Text = Self.CompletionTexts[static_cast<size_t>(Self.SelectedCompletion)] + " ";
		}
		else
		{
			Text = FConsoleManager::Get().CompleteInput(std::string_view(Data->Buf, static_cast<size_t>(Data->BufTextLen)));
		}
		Self.ReplaceText(Data, Text);
		Self.HistoryIndex = -1;
		break;
	}
	case ImGuiInputTextFlags_CallbackHistory:
	{
		const bool bUp = Data->EventKey == ImGuiKey_UpArrow;
		if (!Self.CompletionTexts.empty() && Self.HistoryIndex < 0)
		{
			const int32 Count       = static_cast<int32>(Self.CompletionTexts.size());
			Self.SelectedCompletion = bUp ? (Self.SelectedCompletion <= 0 ? Count - 1 : Self.SelectedCompletion - 1) : (Self.SelectedCompletion + 1) % Count;
			break;
		}
		const std::vector<std::string>& History = FConsoleManager::Get().GetHistory();
		const int32                     Count   = static_cast<int32>(History.size());
		if (Count == 0)
		{
			break;
		}
		Self.HistoryIndex = bUp ? std::min(Self.HistoryIndex + 1, Count - 1) : std::max(Self.HistoryIndex - 1, -1);
		Self.ReplaceText(Data, Self.HistoryIndex >= 0 ? History[static_cast<size_t>(Count - 1 - Self.HistoryIndex)] : std::string());
		break;
	}
	case ImGuiInputTextFlags_CallbackEdit:
		Self.HistoryIndex = -1; // 직접 고치면 기록 탐색 끝 → 후보 다시
		break;
	default:
		break;
	}
	return 0;
}

bool FConsoleInput::Draw()
{
	bool bExecuted = false;
	if (bRequestFocus)
	{
		ImGui::SetKeyboardFocusHere();
		bRequestFocus   = false;
		bHasPendingText = true; // 첫 콜백에서 커서를 끝으로
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	const ImGuiInputTextFlags Flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory |
	                                  ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_EscapeClearsAll;
	const bool bSubmitted = ImGui::InputTextWithHint("##ConsoleInput", ICON_FA_CHEVRON_RIGHT " 콘솔 명령 (help, cvars r., stat fps — Tab 완성, ↑↓ 후보/기록)",
	                                                 Buffer, sizeof(Buffer), Flags, &FConsoleInput::HandleCallback, this);
	const bool   bActive  = ImGui::IsItemActive();
	const ImVec2 InputMin = ImGui::GetItemRectMin();

	if (bSubmitted)
	{
		const std::string Line = Buffer;
		Buffer[0]              = '\0';
		HistoryIndex           = -1;
		if (!Line.empty())
		{
			FConsoleManager::Get().AddHistory(Line);
			FConsoleManager::Get().Execute(Line);
			bExecuted = true;
		}
		bRequestFocus = true; // 이어서 입력
	}

	RefreshCompletions();
	bool bHoveredNow = false;
	if ((bActive || bPopupHovered) && !CompletionTexts.empty())
	{
		// 입력 줄 위로 펼친다 (출력 로그 패널은 보통 화면 아래쪽)
		ImGui::SetNextWindowPos(InputMin, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowBgAlpha(1.0f);
		const ImGuiWindowFlags PopupFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
		                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
		if (ImGui::Begin("##ConsoleCompletions", nullptr, PopupFlags))
		{
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
			if (ImGui::BeginTable("Completions", 2, ImGuiTableFlags_SizingFixedFit))
			{
				for (size_t Index = 0; Index < CompletionTexts.size(); ++Index)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::PushID(static_cast<int>(Index));
					const bool bSelected = static_cast<int32>(Index) == SelectedCompletion;
					if (CompletionIsCommand[Index])
					{
						ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Accent);
					}
					if (ImGui::Selectable(CompletionNames[Index].c_str(), bSelected, ImGuiSelectableFlags_SpanAllColumns))
					{
						SetText(CompletionTexts[Index] + " ", true);
					}
					if (CompletionIsCommand[Index])
					{
						ImGui::PopStyleColor();
					}
					if (bSelected)
					{
						ImGui::SetScrollHereY();
					}
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", CompletionDetails[Index].c_str());
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			bHoveredNow = ImGui::IsWindowHovered();
		}
		ImGui::End();
	}
	bPopupHovered = bHoveredNow;
	return bExecuted;
}

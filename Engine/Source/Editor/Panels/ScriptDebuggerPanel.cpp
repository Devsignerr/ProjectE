#include "Editor/Panels/ScriptDebuggerPanel.h"

#include "Core/Log.h"
#include "Core/Settings/SettingsRegistry.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"

#include <imgui.h>
#include <json.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	using nlohmann::json;

	constexpr const char* GSectionId = "ScriptDebugger";

	std::vector<std::string> ScanScripts(const std::filesystem::path& ContentDirectory)
	{
		std::vector<std::string> Result;
		std::error_code          ErrorCode;
		for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode);
		     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
		{
			if (It->is_regular_file(ErrorCode) && It->path().extension() == L".lua")
			{
				Result.push_back(FStringConv::ToUtf8(std::filesystem::relative(It->path(), ContentDirectory, ErrorCode).generic_wstring()));
			}
		}
		std::sort(Result.begin(), Result.end());
		return Result;
	}

	// 절대 경로 또는 Content 기준 → Content 기준 ('/' 구분). Content 밖이면 빈 문자열
	std::string ToAsset(const FEditorContext& Context, const std::filesystem::path& Path)
	{
		if (!Path.is_absolute())
		{
			return FStringConv::ToUtf8(Path.generic_wstring());
		}
		std::error_code             ErrorCode;
		const std::filesystem::path Relative = std::filesystem::relative(Path, Context.ContentDirectory, ErrorCode);
		if (ErrorCode || Relative.empty() || Relative.native().starts_with(L".."))
		{
			return std::string();
		}
		return FStringConv::ToUtf8(Relative.generic_wstring());
	}

	bool SameAsset(const std::string& A, const std::string& B)
	{
		return FScriptDebugger::NormalizeFile(A) == FScriptDebugger::NormalizeFile(B);
	}
} // namespace

// ---------------------------------------------------------------- 설정 / 저장

void FScriptDebuggerPanel::Initialize(bool bInPersistent)
{
	bPersistent = bInPersistent;
	Debugger.SetPauseHandler([this](FScriptDebugger&) { HandlePause(); });
	if (!bPersistent)
	{
		SavedRevision = Debugger.GetBreakpointRevision();
		return;
	}
	FSettingsRegistry::FDesc Desc{ GSectionId, "스크립트 디버거", "에디터", "Lua 중단점/조사식 (프로젝트별 개인 상태)", ESettingsScope::ProjectUser, false, true };
	FSettingsSection& Section = FSettingsRegistry::Get().RegisterCustom(
		this, Desc,
		[this](std::string_view Text, std::string* Error) {
			const json Document = json::parse(Text, nullptr, false);
			if (Document.is_discarded() || !Document.is_object())
			{
				if (Error != nullptr)
				{
					*Error = "JSON 형식 오류";
				}
				return false;
			}
			Debugger.SetBreakOnError(Document.value("BreakOnError", true));
			std::vector<FScriptBreakpoint> Breakpoints;
			if (const auto Found = Document.find("Breakpoints"); Found != Document.end() && Found->is_array())
			{
				for (const json& Item : *Found)
				{
					FScriptBreakpoint Breakpoint;
					Breakpoint.File      = Item.value("File", std::string());
					Breakpoint.Line      = Item.value("Line", 0);
					Breakpoint.Condition = Item.value("Condition", std::string());
					Breakpoint.bEnabled  = Item.value("Enabled", true);
					if (!Breakpoint.File.empty() && Breakpoint.Line > 0)
					{
						Breakpoints.push_back(std::move(Breakpoint));
					}
				}
			}
			Debugger.SetBreakpoints(std::move(Breakpoints));
			WatchExpressions.clear();
			if (const auto Found = Document.find("Watches"); Found != Document.end() && Found->is_array())
			{
				for (const json& Item : *Found)
				{
					if (Item.is_string())
					{
						WatchExpressions.push_back(Item.get<std::string>());
					}
				}
			}
			return true;
		},
		[this]() {
			json Document;
			Document["BreakOnError"] = Debugger.GetBreakOnError();
			json List                = json::array();
			for (const FScriptBreakpoint& Breakpoint : Debugger.GetBreakpoints())
			{
				List.push_back({ { "File", Breakpoint.File }, { "Line", Breakpoint.Line }, { "Condition", Breakpoint.Condition }, { "Enabled", Breakpoint.bEnabled } });
			}
			Document["Breakpoints"] = std::move(List);
			Document["Watches"]     = WatchExpressions;
			return Document.dump(2);
		},
		[this]() {
			Debugger.ClearBreakpoints();
			Debugger.SetBreakOnError(true);
			WatchExpressions.clear();
		});
	Section.Load();
	SavedRevision = Debugger.GetBreakpointRevision();
}

void FScriptDebuggerPanel::SavePersistentState()
{
	SavedRevision = Debugger.GetBreakpointRevision();
	if (!bPersistent)
	{
		return;
	}
	if (const FSettingsSection* Section = FSettingsRegistry::Get().Find(GSectionId))
	{
		Section->Save();
	}
}

// ---------------------------------------------------------------- 파일

bool FScriptDebuggerPanel::LoadLines(const std::filesystem::path& Path, std::vector<std::string>& OutLines)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		return false;
	}
	std::stringstream Stream;
	Stream << File.rdbuf();
	std::string Text = Stream.str();
	if (Text.size() >= 3 && static_cast<unsigned char>(Text[0]) == 0xEF && static_cast<unsigned char>(Text[1]) == 0xBB && static_cast<unsigned char>(Text[2]) == 0xBF)
	{
		Text.erase(0, 3);
	}
	OutLines.clear();
	std::string Line;
	for (const char Char : Text)
	{
		if (Char == '\n')
		{
			OutLines.push_back(std::move(Line));
			Line.clear();
		}
		else if (Char == '\t')
		{
			Line += "    "; // ImGui는 탭을 펼치지 않는다
		}
		else if (Char != '\r')
		{
			Line += Char;
		}
	}
	if (!Line.empty())
	{
		OutLines.push_back(std::move(Line));
	}
	return true;
}

FScriptDebuggerPanel::FSourceFile* FScriptDebuggerPanel::FindOrLoad(const FEditorContext& Context, const std::string& Asset)
{
	for (FSourceFile& File : Files)
	{
		if (SameAsset(File.Asset, Asset))
		{
			return &File;
		}
	}
	FSourceFile File;
	File.Asset = Asset;
	if (!LoadLines(Context.ContentDirectory / FStringConv::ToWide(Asset), File.Lines))
	{
		return nullptr;
	}
	Files.push_back(std::move(File));
	return &Files.back();
}

void FScriptDebuggerPanel::OpenFile(const FEditorContext& Context, const std::filesystem::path& Path)
{
	const std::string Asset = ToAsset(Context, Path);
	if (Asset.empty() || FindOrLoad(Context, Asset) == nullptr)
	{
		E_LOG(LogEditor, Warning, "스크립트 디버거: 파일을 열 수 없습니다 — {}", FStringConv::ToUtf8(Path.wstring()));
		return;
	}
	ActiveFile       = Asset;
	bSelectActiveTab = true;
	bOpen            = true;
	bFocusWindow     = true;
}

void FScriptDebuggerPanel::OnScriptFileChanged(const FEditorContext& Context, const std::filesystem::path& AbsolutePath)
{
	const std::string Asset = ToAsset(Context, AbsolutePath);
	if (Asset.empty())
	{
		return;
	}
	std::vector<std::string> Lines;
	if (!LoadLines(AbsolutePath, Lines))
	{
		return;
	}
	for (FSourceFile& File : Files)
	{
		if (SameAsset(File.Asset, Asset))
		{
			File.Lines = Lines;
		}
	}
	// 중단점 줄 번호는 그대로 두고, 파일 끝을 넘는 것만 지운다
	if (const int32 Removed = Debugger.PruneBreakpoints(Asset, static_cast<int32>(Lines.size())); Removed > 0)
	{
		E_LOG(LogEditor, Display, "스크립트 디버거: {} 줄 수가 줄어 중단점 {}개 제거", Asset, Removed);
	}
}

// ---------------------------------------------------------------- 정지

void FScriptDebuggerPanel::HandlePause()
{
	++PauseSerial;
	SelectedFrame = Debugger.GetPauseState().CurrentFrame;
	Children.clear();
	const FScriptPauseState& State = Debugger.GetPauseState();
	if (!State.File.empty())
	{
		ActiveFile       = State.File; // 그릴 때 읽는다 (Context 필요)
		bSelectActiveTab = true;
		ScrollToLine = State.Line;
	}
	bOpen            = true;
	bFocusWindow     = true;
	bSelectLocalsTab = true; // 새로 멈추면 지역 변수부터
	if (OnPaused)
	{
		OnPaused(); // 중첩 루프: 재개 명령이 내려질 때까지
	}
	else
	{
		Debugger.Continue();
	}
	Children.clear();
	Locals.clear();
	Upvalues.clear();
	for (FWatch& Watch : Watches)
	{
		Watch.bOk = false;
	}
	ViewSerial = ~0ull;
}

void FScriptDebuggerPanel::RefreshPausedViews()
{
	if (!Debugger.IsPaused())
	{
		return;
	}
	if (ViewSerial == PauseSerial && ViewFrame == SelectedFrame && Watches.size() == WatchExpressions.size())
	{
		return;
	}
	ViewSerial = PauseSerial;
	ViewFrame  = SelectedFrame;
	Children.clear();
	Locals   = Debugger.GetLocals(SelectedFrame);
	Upvalues = Debugger.GetUpvalues(SelectedFrame);
	Watches.clear();
	for (const std::string& Expression : WatchExpressions)
	{
		FWatch Watch;
		Watch.Expression = Expression;
		Watch.bOk        = Debugger.Evaluate(SelectedFrame, Expression, Watch.Result);
		Watches.push_back(std::move(Watch));
	}
}

void FScriptDebuggerPanel::HandlePausedShortcuts()
{
	if (!Debugger.IsPaused() || ImGui::GetIO().WantTextInput)
	{
		return;
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F11))
	{
		Debugger.StepOut();
	}
	else if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && !ImGui::GetIO().KeyShift)
	{
		Debugger.StepInto();
	}
	else if (ImGui::IsKeyPressed(ImGuiKey_F10, false))
	{
		Debugger.StepOver();
	}
	else if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
	{
		Debugger.Continue();
	}
}

// ---------------------------------------------------------------- 그리기

void FScriptDebuggerPanel::Draw(FEditorContext& Context)
{
	// 중단점 목록이 바뀌면 저장 (조작 한 번마다 — 작은 파일)
	if (Debugger.GetBreakpointRevision() != SavedRevision)
	{
		SavePersistentState();
	}
	if (!bOpen)
	{
		return;
	}
	if (bFocusWindow && ImGui::GetFrameCount() > 3)
	{
		ImGui::SetNextWindowFocus();
		bFocusWindow = false;
	}
	ImGui::SetNextWindowSize(ImVec2(900.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_BUG, "스크립트 디버거", "ScriptDebugger").c_str(), &bOpen))
	{
		ImGui::End();
		return;
	}
	RefreshPausedViews();
	DrawToolbar(Context);
	ImGui::Separator();

	const ImVec2 Avail       = ImGui::GetContentRegionAvail();
	const float  SourceWidth = std::max(200.0f, Avail.x * 0.58f);
	if (ImGui::BeginChild("SourceArea", ImVec2(SourceWidth, 0.0f)))
	{
		DrawSource(Context);
	}
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("InfoArea", ImVec2(0.0f, 0.0f)))
	{
		if (ImGui::BeginTabBar("DebuggerTabs"))
		{
			if (ImGui::BeginTabItem(ICON_FA_LAYER_GROUP " 호출 스택"))
			{
				DrawCallStack(Context);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(ICON_FA_LIST " 지역 변수", nullptr, bSelectLocalsTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
			{
				DrawVariables();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(ICON_FA_EYE " 조사식"))
			{
				DrawWatches();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(ICON_FA_CIRCLE " 중단점"))
			{
				DrawBreakpoints(Context);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
			bSelectLocalsTab = false;
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

void FScriptDebuggerPanel::DrawToolbar(FEditorContext& Context)
{
	const bool bPaused = Debugger.IsPaused();
	ImGui::BeginDisabled(!bPaused);
	if (FEditorTheme::ToolButton(ICON_FA_PLAY, "계속 (F5)", false))
	{
		Debugger.Continue();
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_ARROW_RIGHT_LONG, "프로시저 단위 실행 — 넘기기 (F10)", false))
	{
		Debugger.StepOver();
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_ARROW_TURN_DOWN, "한 단계씩 실행 — 들어가기 (F11)", false))
	{
		Debugger.StepInto();
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_ARROW_TURN_UP, "프로시저 나가기 (Shift+F11)", false))
	{
		Debugger.StepOut();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(bPaused || !Context.bPlaying);
	if (FEditorTheme::ToolButton(ICON_FA_PAUSE, "모두 중단: 다음 스크립트 줄에서 멈춤", false))
	{
		Debugger.RequestPause();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!Context.bPlaying);
	ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Danger);
	if (FEditorTheme::ToolButton(ICON_FA_STOP, "플레이 정지 (정지 중이면 재개 후 정지 — 남은 코드는 중단점 없이 끝까지)", false) && OnStopPlay)
	{
		OnStopPlay();
	}
	ImGui::PopStyleColor();
	ImGui::EndDisabled();

	ImGui::SameLine();
	bool bBreakOnError = Debugger.GetBreakOnError();
	if (ImGui::Checkbox("오류 시 멈춤", &bBreakOnError))
	{
		Debugger.SetBreakOnError(bBreakOnError);
		SavePersistentState();
	}
	ImGui::SetItemTooltip("처리되지 않은 스크립트 오류가 나면 그 자리에서 멈추고 스택을 보여 준다 (pcall로 잡은 오류는 제외)");

	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_FOLDER_OPEN " 열기"))
	{
		ScriptList = ScanScripts(Context.ContentDirectory);
		ImGui::OpenPopup("OpenScript");
	}
	if (ImGui::BeginPopup("OpenScript"))
	{
		if (ScriptList.empty())
		{
			ImGui::TextDisabled("Content에 .lua 파일이 없습니다");
		}
		for (const std::string& Script : ScriptList)
		{
			if (ImGui::MenuItem(Script.c_str()))
			{
				OpenFile(Context, FStringConv::ToWide(Script));
			}
		}
		ImGui::EndPopup();
	}

	// 상태
	ImGui::SameLine();
	if (bPaused)
	{
		const FScriptPauseState& State = Debugger.GetPauseState();
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_PAUSE " 정지됨 (%s) %s:%d%s", ToString(State.Reason), State.File.c_str(), State.Line,
		                   State.bInCoroutine ? " [코루틴]" : "");
	}
	else if (!Context.bPlaying)
	{
		ImGui::TextDisabled("플레이 중이 아님 — 중단점은 다음 플레이부터");
	}
	else if (!Debugger.IsEnabled())
	{
		ImGui::TextDisabled("디버거 꺼짐");
	}
	else
	{
		ImGui::TextColored(FEditorTheme::Success, "실행 중");
	}
	if (bPaused && !Debugger.GetPauseState().Message.empty())
	{
		const std::string& Message = Debugger.GetPauseState().Message;
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_BUG " %s", Message.substr(0, Message.find('\n')).c_str());
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", Message.c_str());
		}
	}
}

void FScriptDebuggerPanel::DrawSource(FEditorContext& Context)
{
	// 파일 탭
	if (!ActiveFile.empty())
	{
		FindOrLoad(Context, ActiveFile);
	}
	if (Files.empty())
	{
		ImGui::TextDisabled("열린 스크립트가 없습니다. '열기' 또는 콘텐츠 브라우저에서 .lua를 더블클릭하세요.");
		return;
	}
	int32 CloseIndex = -1;
	if (ImGui::BeginTabBar("SourceTabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll))
	{
		for (size_t Index = 0; Index < Files.size(); ++Index)
		{
			bool                    bTabOpen = true;
			const bool              bActive  = SameAsset(Files[Index].Asset, ActiveFile);
			// 탭 제목 = 파일 이름, ID = Content 기준 경로
			const std::string       Label = FStringConv::ToUtf8(std::filesystem::path(FStringConv::ToWide(Files[Index].Asset)).filename().wstring()) + "###" + Files[Index].Asset;
			const ImGuiTabItemFlags Flags = bActive && bSelectActiveTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
			if (ImGui::BeginTabItem(Label.c_str(), &bTabOpen, Flags))
			{
				// 코드가 다른 탭을 고른 직후(SetSelected는 다음 프레임에 반영)에는 지금 보이는 탭으로 되돌리지 않는다
				if (!bSelectActiveTab || bActive)
				{
					ActiveFile       = Files[Index].Asset;
					bSelectActiveTab = false;
				}
				ImGui::EndTabItem();
			}
			ImGui::SetItemTooltip("%s", Files[Index].Asset.c_str());
			if (!bTabOpen)
			{
				CloseIndex = static_cast<int32>(Index);
			}
		}
		ImGui::EndTabBar();
	}
	if (CloseIndex >= 0)
	{
		const bool bWasActive = SameAsset(Files[static_cast<size_t>(CloseIndex)].Asset, ActiveFile);
		Files.erase(Files.begin() + CloseIndex);
		if (bWasActive)
		{
			ActiveFile = Files.empty() ? std::string() : Files.front().Asset;
		}
		return;
	}
	FSourceFile* File = ActiveFile.empty() ? nullptr : FindOrLoad(Context, ActiveFile);
	if (File == nullptr)
	{
		return;
	}

	// 정지 줄 (선택한 프레임)
	int32 PausedLine  = 0;
	bool  bTopFrame   = true;
	if (Debugger.IsPaused())
	{
		const FScriptPauseState& State = Debugger.GetPauseState();
		if (SelectedFrame >= 0 && SelectedFrame < static_cast<int32>(State.Stack.size()))
		{
			const FScriptStackFrame& Frame = State.Stack[static_cast<size_t>(SelectedFrame)];
			if (Frame.bFile && SameAsset(Frame.File, File->Asset))
			{
				PausedLine = Frame.Line;
				bTopFrame  = SelectedFrame == State.CurrentFrame;
			}
		}
	}

	if (!ImGui::BeginChild("SourceLines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
	{
		ImGui::EndChild();
		return;
	}
	const float LineHeight = ImGui::GetTextLineHeight();
	if (ScrollToLine > 0 && !bSelectActiveTab)
	{
		ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(ScrollToLine) - 6.0f) * LineHeight));
		ScrollToLine = 0;
	}
	const std::string Digits      = std::to_string(File->Lines.size());
	const float       GutterWidth = ImGui::CalcTextSize(Digits.c_str()).x + LineHeight * 2.0f;
	ImDrawList*       DrawList    = ImGui::GetWindowDrawList();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
	ImGuiListClipper Clipper;
	Clipper.Begin(static_cast<int>(File->Lines.size()), LineHeight);
	while (Clipper.Step())
	{
		for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
		{
			const int32  Line   = Row + 1;
			const ImVec2 Origin = ImGui::GetCursorScreenPos();
			const float  Width  = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetWindowWidth());
			if (Line == PausedLine)
			{
				const ImVec4 Color = bTopFrame ? ImVec4(FEditorTheme::Warning.x, FEditorTheme::Warning.y, FEditorTheme::Warning.z, 0.28f)
				                               : ImVec4(FEditorTheme::Accent.x, FEditorTheme::Accent.y, FEditorTheme::Accent.z, 0.30f);
				DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Width + ImGui::GetScrollX() + 4000.0f, Origin.y + LineHeight), ImGui::GetColorU32(Color));
			}

			// 여백: 클릭 = 중단점 토글, 우클릭 = 조건/끄기
			ImGui::PushID(Row);
			ImGui::InvisibleButton("##Gutter", ImVec2(GutterWidth, LineHeight));
			const bool bGutterHovered = ImGui::IsItemHovered();
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				Debugger.ToggleBreakpoint(File->Asset, Line);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{
				const FScriptBreakpoint* Existing = Debugger.FindBreakpoint(File->Asset, Line);
				if (Existing == nullptr)
				{
					Debugger.ToggleBreakpoint(File->Asset, Line);
					Existing = Debugger.FindBreakpoint(File->Asset, Line);
				}
				ConditionLine = Line;
				std::snprintf(ConditionBuffer, sizeof(ConditionBuffer), "%s", Existing != nullptr ? Existing->Condition.c_str() : "");
				ImGui::OpenPopup("BreakpointCondition");
			}
			if (ImGui::BeginPopup("BreakpointCondition"))
			{
				ImGui::Text("%s:%d 중단점", File->Asset.c_str(), ConditionLine);
				ImGui::SetNextItemWidth(320.0f);
				if (ImGui::InputTextWithHint("##Condition", "조건식 (예: self.Health < 10) — 비우면 항상", ConditionBuffer, sizeof(ConditionBuffer),
				                             ImGuiInputTextFlags_EnterReturnsTrue))
				{
					Debugger.SetBreakpointCondition(File->Asset, ConditionLine, ConditionBuffer);
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::Button("적용"))
				{
					Debugger.SetBreakpointCondition(File->Asset, ConditionLine, ConditionBuffer);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (const FScriptBreakpoint* Existing = Debugger.FindBreakpoint(File->Asset, ConditionLine))
				{
					if (ImGui::Button(Existing->bEnabled ? "끄기" : "켜기"))
					{
						Debugger.SetBreakpointEnabled(File->Asset, ConditionLine, !Existing->bEnabled);
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
				}
				if (ImGui::Button(ICON_FA_TRASH " 제거"))
				{
					Debugger.RemoveBreakpoint(File->Asset, ConditionLine);
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();

			const FScriptBreakpoint* Breakpoint = Debugger.FindBreakpoint(File->Asset, Line);
			const ImVec2             Center(Origin.x + LineHeight * 0.55f, Origin.y + LineHeight * 0.5f);
			if (Breakpoint != nullptr)
			{
				const ImU32 Color = Breakpoint->bEnabled ? ImGui::GetColorU32(FEditorTheme::Danger) : ImGui::GetColorU32(FEditorTheme::TextDim);
				DrawList->AddCircleFilled(Center, LineHeight * 0.32f, Color);
				if (!Breakpoint->Condition.empty())
				{
					DrawList->AddCircle(Center, LineHeight * 0.32f, IM_COL32(255, 255, 255, 220), 0, 1.5f);
				}
			}
			else if (bGutterHovered)
			{
				DrawList->AddCircleFilled(Center, LineHeight * 0.32f, ImGui::GetColorU32(ImVec4(FEditorTheme::Danger.x, FEditorTheme::Danger.y, FEditorTheme::Danger.z, 0.35f)));
			}
			if (Line == PausedLine)
			{
				// 현재 줄 화살표
				const float X = Origin.x + LineHeight * 1.05f;
				DrawList->AddTriangleFilled(ImVec2(X, Origin.y + 2.0f), ImVec2(X, Origin.y + LineHeight - 2.0f), ImVec2(X + LineHeight * 0.45f, Origin.y + LineHeight * 0.5f),
				                            ImGui::GetColorU32(bTopFrame ? FEditorTheme::Warning : FEditorTheme::Accent));
			}
			const std::string Number = std::to_string(Line);
			DrawList->AddText(ImVec2(Origin.x + GutterWidth - ImGui::CalcTextSize(Number.c_str()).x - 6.0f, Origin.y), ImGui::GetColorU32(FEditorTheme::TextDim),
			                  Number.c_str());
			ImGui::SameLine(0.0f, 8.0f);
			ImGui::TextUnformatted(File->Lines[static_cast<size_t>(Row)].c_str());
		}
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();
}

void FScriptDebuggerPanel::DrawCallStack(FEditorContext& Context)
{
	if (!Debugger.IsPaused())
	{
		ImGui::TextDisabled("정지 중이 아닙니다");
		return;
	}
	const FScriptPauseState& State = Debugger.GetPauseState();
	for (size_t Index = 0; Index < State.Stack.size(); ++Index)
	{
		const FScriptStackFrame& Frame = State.Stack[Index];
		const std::string        Label = Frame.bFile ? std::format("{}  —  {}:{}", Frame.Function, Frame.File, Frame.Line) : std::format("{}  ({})", Frame.Function, Frame.File);
		ImGui::PushID(static_cast<int>(Index));
		if (!Frame.bFile)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::TextDim);
		}
		if (ImGui::Selectable(Label.c_str(), SelectedFrame == static_cast<int32>(Index)))
		{
			SelectedFrame = static_cast<int32>(Index);
			if (Frame.bFile)
			{
				OpenFile(Context, FStringConv::ToWide(Frame.File));
				ScrollToLine = Frame.Line;
				bFocusWindow = false;
			}
		}
		if (!Frame.bFile)
		{
			ImGui::PopStyleColor();
		}
		ImGui::PopID();
	}
}

void FScriptDebuggerPanel::DrawVariableRow(const FScriptVariable& Variable, int32 Depth)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	bool bOpenNode = false;
	if (Variable.ChildHandle != 0 && Depth < 32)
	{
		ImGui::PushID(static_cast<int>(Variable.ChildHandle));
		bOpenNode = ImGui::TreeNodeEx(Variable.Name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
		ImGui::PopID();
	}
	else
	{
		ImGui::TreeNodeEx(Variable.Name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
	}
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(Variable.Value.c_str());
	if (ImGui::IsItemHovered() && Variable.Value.size() > 40)
	{
		ImGui::SetTooltip("%s", Variable.Value.c_str());
	}
	ImGui::TableSetColumnIndex(2);
	ImGui::TextDisabled("%s", Variable.Type.c_str());
	if (bOpenNode)
	{
		auto Found = Children.find(Variable.ChildHandle);
		if (Found == Children.end())
		{
			Found = Children.emplace(Variable.ChildHandle, Debugger.GetChildren(Variable.ChildHandle)).first;
		}
		// 펼칠 때 받은 목록 사본으로 그린다 (아래 펼치기가 맵을 바꿀 수 있다)
		const std::vector<FScriptVariable> Items = Found->second;
		for (size_t Index = 0; Index < Items.size(); ++Index)
		{
			ImGui::PushID(static_cast<int>(Index)); // 같은 이름 항목 구분
			DrawVariableRow(Items[Index], Depth + 1);
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
}

void FScriptDebuggerPanel::DrawVariables()
{
	if (!Debugger.IsPaused())
	{
		ImGui::TextDisabled("정지 중이 아닙니다");
		return;
	}
	if (ImGui::BeginTable("Locals", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 0.35f);
		ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("타입", ImGuiTableColumnFlags_WidthStretch, 0.15f);
		ImGui::TableHeadersRow();
		for (size_t Index = 0; Index < Locals.size(); ++Index)
		{
			ImGui::PushID(static_cast<int>(Index)); // 같은 이름 지역 변수(가린 변수) 구분
			DrawVariableRow(Locals[Index], 0);
			ImGui::PopID();
		}
		if (!Upvalues.empty())
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextColored(FEditorTheme::Accent, "upvalue");
			for (size_t Index = 0; Index < Upvalues.size(); ++Index)
			{
				ImGui::PushID(static_cast<int>(10000 + Index));
				DrawVariableRow(Upvalues[Index], 0);
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
}

void FScriptDebuggerPanel::DrawWatches()
{
	ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 정지 프레임에서 실제로 실행됩니다 — 함수 호출·테이블 수정은 게임 상태를 바꿉니다");
	ImGui::SetNextItemWidth(-80.0f);
	const bool bEnter = ImGui::InputTextWithHint("##NewWatch", "식 추가 (예: self.Properties.Speed * 2)", NewWatch, sizeof(NewWatch), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if ((ImGui::Button("추가") || bEnter) && NewWatch[0] != '\0')
	{
		WatchExpressions.emplace_back(NewWatch);
		NewWatch[0] = '\0';
		ViewSerial  = ~0ull; // 다시 평가
		SavePersistentState();
	}
	int32 RemoveIndex = -1;
	if (ImGui::BeginTable("Watches", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("식", ImGuiTableColumnFlags_WidthStretch, 0.4f);
		ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch, 0.6f);
		ImGui::TableSetupColumn("##Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		for (size_t Index = 0; Index < WatchExpressions.size(); ++Index)
		{
			ImGui::PushID(static_cast<int>(Index));
			const FWatch* Watch = Debugger.IsPaused() && Index < Watches.size() ? &Watches[Index] : nullptr;
			if (Watch != nullptr && Watch->bOk)
			{
				FScriptVariable Row = Watch->Result;
				Row.Name            = WatchExpressions[Index];
				DrawVariableRow(Row, 0);
				ImGui::TableSetColumnIndex(2);
			}
			else
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(WatchExpressions[Index].c_str());
				ImGui::TableSetColumnIndex(1);
				if (Watch == nullptr)
				{
					ImGui::TextDisabled("(정지 중에 평가)");
				}
				else
				{
					ImGui::TextColored(FEditorTheme::Danger, "%s", Watch->Result.Value.c_str());
				}
				ImGui::TableSetColumnIndex(2);
			}
			if (ImGui::SmallButton(ICON_FA_XMARK))
			{
				RemoveIndex = static_cast<int32>(Index);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (RemoveIndex >= 0)
	{
		WatchExpressions.erase(WatchExpressions.begin() + RemoveIndex);
		ViewSerial = ~0ull;
		SavePersistentState();
	}
}

void FScriptDebuggerPanel::DrawBreakpoints(FEditorContext& Context)
{
	const std::vector<FScriptBreakpoint>& Breakpoints = Debugger.GetBreakpoints();
	if (Breakpoints.empty())
	{
		ImGui::TextDisabled("중단점이 없습니다. 소스 왼쪽 여백을 클릭하세요 (우클릭 = 조건).");
		return;
	}
	if (ImGui::SmallButton(ICON_FA_TRASH " 모두 제거"))
	{
		Debugger.ClearBreakpoints();
		return;
	}
	// 조작은 표를 다 그린 뒤 적용한다 (목록이 바뀌면 참조가 무효)
	std::function<void()> Pending;
	if (ImGui::BeginTable("BreakpointList", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
	{
		ImGui::TableSetupColumn("##On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("위치", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("조건 / 적중", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("##Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		for (size_t Index = 0; Index < Breakpoints.size(); ++Index)
		{
			const FScriptBreakpoint& Breakpoint = Breakpoints[Index];
			ImGui::PushID(static_cast<int>(Index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			bool bEnabled = Breakpoint.bEnabled;
			if (ImGui::Checkbox("##Enabled", &bEnabled))
			{
				Pending = [this, File = Breakpoint.File, Line = Breakpoint.Line, bEnabled]() { Debugger.SetBreakpointEnabled(File, Line, bEnabled); };
			}
			ImGui::TableSetColumnIndex(1);
			const std::string Location = std::format("{}:{}", Breakpoint.File, Breakpoint.Line);
			if (ImGui::Selectable(Location.c_str()))
			{
				Pending = [this, &Context, File = Breakpoint.File, Line = Breakpoint.Line]() {
					OpenFile(Context, FStringConv::ToWide(File));
					ScrollToLine = Line;
				};
			}
			ImGui::TableSetColumnIndex(2);
			if (!Breakpoint.Condition.empty())
			{
				ImGui::TextColored(FEditorTheme::Accent, "%s", Breakpoint.Condition.c_str());
				ImGui::SameLine();
			}
			ImGui::TextDisabled("적중 %u", Breakpoint.HitCount);
			ImGui::TableSetColumnIndex(3);
			if (ImGui::SmallButton(ICON_FA_XMARK))
			{
				Pending = [this, File = Breakpoint.File, Line = Breakpoint.Line]() { Debugger.RemoveBreakpoint(File, Line); };
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (Pending)
	{
		Pending();
	}
}

#include "Editor/AssetEditors/DataTableEditor.h"

#include "Core/CommandLine.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/DataTableView.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Scene/DataCsv.h"
#include "Scene/DataLibrary.h"
#include "Scene/Prefab.h"

#include <commdlg.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::shared_ptr<const FDataStruct> ResolveStruct(const std::string& StructPath)
	{
		return FDataLibrary::Get().LoadStruct(StructPath);
	}

	uint64 MakeCellKey(int32 RowIndex, int32 FieldIndex)
	{
		return (static_cast<uint64>(static_cast<uint32>(RowIndex)) << 32) | static_cast<uint32>(FieldIndex);
	}

	// Win32 CSV 파일 대화상자 (취소하면 빈 경로). 모달이라 자동 검증 경로에서는 부르지 않는다
	std::filesystem::path ShowCsvDialog(const std::filesystem::path& InitialDirectory, const std::wstring& DefaultName, bool bSave)
	{
		wchar_t Buffer[MAX_PATH] = {};
		wcsncpy_s(Buffer, DefaultName.c_str(), _TRUNCATE);
		const std::wstring InitialDir = InitialDirectory.wstring();
		OPENFILENAMEW      Dialog{};
		Dialog.lStructSize     = sizeof(Dialog);
		Dialog.hwndOwner       = GetActiveWindow();
		Dialog.lpstrFilter     = L"CSV/TSV (*.csv;*.tsv;*.txt)\0*.csv;*.tsv;*.txt\0모든 파일 (*.*)\0*.*\0";
		Dialog.lpstrFile       = Buffer;
		Dialog.nMaxFile        = MAX_PATH;
		Dialog.lpstrInitialDir = InitialDir.c_str();
		Dialog.lpstrDefExt     = L"csv";
		Dialog.Flags           = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (bSave ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
		const BOOL bOk         = bSave ? GetSaveFileNameW(&Dialog) : GetOpenFileNameW(&Dialog);
		return bOk ? std::filesystem::path(Buffer) : std::filesystem::path();
	}

	bool ReadDiskText(const std::filesystem::path& File, std::string& Out)
	{
		std::ifstream Stream(File, std::ios::binary);
		if (!Stream)
		{
			return false;
		}
		std::stringstream Buffer;
		Buffer << Stream.rdbuf();
		Out = Buffer.str();
		return true;
	}

	// 검증 경고 "행 '이름' 필드 ..." → 행 이름 (없으면 빈 문자열)
	std::string ExtractRowName(const std::string& Warning)
	{
		static constexpr std::string_view Prefix = "행 '";
		if (Warning.rfind(Prefix, 0) != 0)
		{
			return {};
		}
		const size_t End = Warning.find("' 필드 '", Prefix.size());
		return End == std::string::npos ? std::string() : Warning.substr(Prefix.size(), End - Prefix.size());
	}
} // namespace

// ---------------------------------------------------------------- 상태

bool FDataTableEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		Notify(Env, "데이터 테이블을 읽지 못했습니다: " + GetDisplayName(), true);
		return false;
	}
	FDataTable      Loaded;
	FDataLoadReport Report;
	std::string     Error;
	if (!FDataTable::FromJsonString(Text, ResolveStruct, Loaded, &Report, &Error))
	{
		Notify(Env, std::format("데이터 테이블 형식 오류 ({}): {}", GetDisplayName(), Error), true);
		return false;
	}
	Table        = std::move(Loaded);
	LoadWarnings = std::move(Report.Warnings);
	++Revision;
	RememberDiskState();
	if (!Table.HasRow(SelectedRow))
	{
		SelectedRow.clear();
	}
	RenamingRow.clear();

	if (!bAutoArgsRead)
	{
		bAutoArgsRead                  = true;
		const FCommandLine CommandLine = FCommandLine::FromProcess();
		if (const std::string Select = FStringConv::ToUtf8(CommandLine.GetValue(L"--data-select")); Table.HasRow(Select))
		{
			SelectedRow       = Select;
			bScrollToSelected = true;
		}
		AutoPopupField = FStringConv::ToUtf8(CommandLine.GetValue(L"--data-popup"));
	}
	return true;
}

bool FDataTableEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	std::string Error;
	if (!FDataLibrary::Get().SaveTable(GetAssetPathString(), Table, &Error))
	{
		Notify(Env, "데이터 테이블 저장 실패: " + Error, true);
		return false;
	}
	RememberDiskState();
	return true;
}

std::string FDataTableEditor::CaptureState() const
{
	return Table.ToJsonString();
}

void FDataTableEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	FDataTable      Restored;
	FDataLoadReport Report;
	if (!FDataTable::FromJsonString(State, ResolveStruct, Restored, &Report))
	{
		return;
	}
	Table        = std::move(Restored);
	LoadWarnings = std::move(Report.Warnings);
	++Revision;
	if (!Table.HasRow(SelectedRow))
	{
		SelectedRow.clear();
	}
}

void FDataTableEditor::OnDataLibraryChanged(FAssetEditorEnvironment& Env)
{
	// 구조체가 바뀌었을 수 있다 → 같은 내용으로 다시 묶고, 참조 대상 테이블이 바뀌었을 수 있으니 다시 검증
	RestoreState(Env, CaptureState());
}

void FDataTableEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	FDataEditorBase::Update(Env, DeltaSeconds);
	++FrameCounter;
	if (FrameCounter == 20 && FCommandLine::FromProcess().HasFlag(L"--verify-data-roundtrip"))
	{
		RunVerifyRoundTrip(Env);
	}
}

// ---------------------------------------------------------------- 편집

void FDataTableEditor::Edited(std::string_view Label)
{
	MarkEdited(Label);
	++Revision;
}

void FDataTableEditor::AddRow(int32 Index)
{
	std::string Error;
	if (FDataRow* Row = Table.AddRow(Table.MakeUniqueRowName("NewRow"), Index, &Error))
	{
		SelectedRow       = Row->Name;
		RenamingRow       = Row->Name;
		bFocusRename      = true;
		bScrollToSelected = true;
		Edited("행 추가");
	}
}

void FDataTableEditor::DuplicateSelected(FAssetEditorEnvironment& Env)
{
	if (!Table.HasRow(SelectedRow))
	{
		return;
	}
	std::string Error;
	if (FDataRow* Row = Table.DuplicateRow(SelectedRow, Table.MakeUniqueRowName(SelectedRow), &Error))
	{
		SelectedRow       = Row->Name;
		bScrollToSelected = true;
		Edited("행 복제");
	}
	else
	{
		Notify(Env, "행을 복제하지 못했습니다: " + Error, true);
	}
}

void FDataTableEditor::DeleteRow(const std::string& RowName)
{
	const int32 Index = Table.FindRowIndex(RowName);
	if (Index < 0 || !Table.RemoveRow(RowName))
	{
		return;
	}
	if (SelectedRow == RowName)
	{
		const std::vector<FDataRow>& Rows = Table.GetRows();
		SelectedRow = Rows.empty() ? std::string() : Rows[static_cast<size_t>(std::min(Index, Table.GetRowCount() - 1))].Name;
	}
	Edited("행 삭제");
}

bool FDataTableEditor::RenameRow(FAssetEditorEnvironment& Env, const std::string& From, const std::string& To)
{
	if (From == To)
	{
		return true;
	}
	std::string Error;
	if (!Table.RenameRow(From, To, &Error))
	{
		Notify(Env, "행 이름을 바꾸지 못했습니다: " + Error, true);
		return false;
	}
	// 이 테이블 안에서 자기 행을 가리키던 RowRef도 함께 (다른 파일의 참조는 검증 경고로 보인다)
	const int32 Updated = DataTableView::RenameRowReferences(Table, GetSelfReferenceFields(), From, To);
	if (Updated > 0)
	{
		Notify(Env, std::format("행 이름 변경: 이 테이블 안 참조 {}개도 바꿈", Updated), false);
	}
	if (SelectedRow == From)
	{
		SelectedRow = To;
	}
	Edited("행 이름 바꾸기");
	return true;
}

void FDataTableEditor::MoveRow(int32 From, int32 To)
{
	if (Table.MoveRow(From, To))
	{
		bScrollToSelected = true;
		Edited("행 이동");
	}
}

void FDataTableEditor::ChangeStruct(const std::string& StructPath)
{
	if (StructPath == Table.StructPath)
	{
		return;
	}
	Table.StructPath = StructPath;
	Table.Rebind(ResolveStruct(StructPath));
	LoadWarnings.clear();
	if (Table.Struct == nullptr)
	{
		LoadWarnings.push_back("구조체를 찾을 수 없습니다: " + StructPath);
	}
	Edited("구조체 바꾸기");
}

void FDataTableEditor::ApplySortOrder()
{
	if (SortColumn < 0)
	{
		return;
	}
	Table.SetRows(DataTableView::SortedRows(Table, SortColumn, bSortDescending));
	Edited("정렬 적용");
}

std::vector<int32> FDataTableEditor::GetSelfReferenceFields() const
{
	std::vector<int32> Fields;
	if (Table.Struct == nullptr)
	{
		return Fields;
	}
	for (size_t Index = 0; Index < Table.Struct->Fields.size(); ++Index)
	{
		const FDataField&    Field     = Table.Struct->Fields[Index];
		const EDataFieldType ValueType = Field.Type == EDataFieldType::Array ? Field.ElementType : Field.Type;
		if (ValueType == EDataFieldType::RowRef && DataValueWidgets::IsSameDataFile(Field.Table, Path))
		{
			Fields.push_back(static_cast<int32>(Index));
		}
	}
	return Fields;
}

// ---------------------------------------------------------------- 보기 / 검증

void FDataTableEditor::RebuildViewIfNeeded()
{
	if (ViewRevision == Revision && ViewFilter == Filter && ViewSortColumn == SortColumn && bViewDescending == bSortDescending)
	{
		return;
	}
	View            = DataTableView::BuildView(Table, Filter, SortColumn, bSortDescending);
	ViewRevision    = Revision;
	ViewFilter      = Filter;
	ViewSortColumn  = SortColumn;
	bViewDescending = bSortDescending;
}

void FDataTableEditor::Revalidate()
{
	ValidatedRevision = Revision;
	CellProblems.clear();
	ReferenceWarnings.clear();
	if (Table.Struct == nullptr)
	{
		return;
	}
	FDataLibrary&     Library  = FDataLibrary::Get();
	const std::string SelfPath = GetAssetPathString();
	ReferenceWarnings          = Library.ValidateReferences(Table, SelfPath);
	const std::vector<FDataRow>& Rows = Table.GetRows();
	for (size_t RowIndex = 0; RowIndex < Rows.size(); ++RowIndex)
	{
		for (FDataReferenceIssue& Issue : Library.ValidateRecordReferences(*Table.Struct, Rows[RowIndex].Record, SelfPath, &Table))
		{
			std::string& Message = CellProblems[MakeCellKey(static_cast<int32>(RowIndex), Issue.Field)];
			Message              = Message.empty() ? std::move(Issue.Message) : Message + "\n" + Issue.Message;
		}
	}
}

const std::string* FDataTableEditor::FindCellProblem(int32 RowIndex, int32 FieldIndex) const
{
	if (CellProblems.empty())
	{
		return nullptr;
	}
	const auto Found = CellProblems.find(MakeCellKey(RowIndex, FieldIndex));
	return Found == CellProblems.end() ? nullptr : &Found->second;
}

// ---------------------------------------------------------------- CSV

bool FDataTableEditor::ExportCsvTo(const std::filesystem::path& File, std::string* OutError) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(File.parent_path(), ErrorCode);
	std::ofstream Stream(File, std::ios::binary | std::ios::trunc);
	if (Stream)
	{
		Stream << DataCsv::Export(Table);
	}
	if (!Stream)
	{
		if (OutError != nullptr)
		{
			*OutError = "파일을 쓸 수 없습니다: " + FStringConv::ToUtf8(File.generic_wstring());
		}
		return false;
	}
	return true;
}

bool FDataTableEditor::ReadCsvRows(const std::filesystem::path& File, std::vector<FDataRow>& OutRows, std::vector<std::string>& OutWarnings,
                                   std::string* OutError) const
{
	std::string Text;
	if (!ReadDiskText(File, Text))
	{
		if (OutError != nullptr)
		{
			*OutError = "파일을 읽을 수 없습니다: " + FStringConv::ToUtf8(File.generic_wstring());
		}
		return false;
	}
	FDataLoadReport Report;
	if (!DataCsv::Import(Text, Table, OutRows, &Report, OutError))
	{
		return false;
	}
	OutWarnings = std::move(Report.Warnings);
	return true;
}

void FDataTableEditor::ExportCsv(FAssetEditorEnvironment& Env)
{
	const std::filesystem::path File = ShowCsvDialog(Path.parent_path(), Path.stem().wstring() + L".csv", true);
	if (File.empty())
	{
		return;
	}
	std::string Error;
	if (ExportCsvTo(File, &Error))
	{
		Notify(Env, std::format("CSV 내보내기: {} ({}행)", FStringConv::ToUtf8(File.filename().wstring()), Table.GetRowCount()), false);
	}
	else
	{
		Notify(Env, Error, true);
	}
}

void FDataTableEditor::BeginImportCsv(FAssetEditorEnvironment& Env)
{
	const std::filesystem::path File = ShowCsvDialog(Path.parent_path(), Path.stem().wstring() + L".csv", false);
	if (File.empty())
	{
		return;
	}
	std::string Error;
	PendingImportRows.clear();
	PendingImportWarnings.clear();
	if (!ReadCsvRows(File, PendingImportRows, PendingImportWarnings, &Error))
	{
		Notify(Env, "CSV 가져오기 실패: " + Error, true);
		return;
	}
	PendingImportFile = FStringConv::ToUtf8(File.filename().wstring());
	bOpenImportPopup  = true;
}

void FDataTableEditor::DrawImportPopup(FAssetEditorEnvironment& Env)
{
	if (bOpenImportPopup)
	{
		ImGui::OpenPopup("CSV 가져오기##DataTableImport");
		bOpenImportPopup = false;
	}
	if (!ImGui::BeginPopupModal("CSV 가져오기##DataTableImport", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}
	ImGui::Text("%s: %d행", PendingImportFile.c_str(), static_cast<int32>(PendingImportRows.size()));
	if (!PendingImportWarnings.empty())
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 경고 %d개", static_cast<int32>(PendingImportWarnings.size()));
		if (ImGui::BeginChild("##ImportWarnings", ImVec2(ImGui::GetFontSize() * 36.0f, ImGui::GetTextLineHeightWithSpacing() * 6.0f), ImGuiChildFlags_Borders))
		{
			for (const std::string& Warning : PendingImportWarnings)
			{
				ImGui::TextWrapped("%s", Warning.c_str());
			}
		}
		ImGui::EndChild();
	}
	ImGui::TextDisabled("교체: 모든 행을 CSV 내용으로 바꿉니다\n병합: 같은 이름 행은 CSV 값으로 바꾸고 새 행은 끝에 덧붙입니다");
	bool bDone    = false;
	bool bApplied = false;
	if (ImGui::Button("교체"))
	{
		Table.SetRows(std::move(PendingImportRows));
		Edited("CSV 가져오기 (교체)");
		bDone = bApplied = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("병합"))
	{
		Table.SetRows(DataTableView::MergeRows(Table.GetRows(), PendingImportRows));
		Edited("CSV 가져오기 (병합)");
		bDone = bApplied = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("취소") || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		bDone = true;
	}
	if (bDone)
	{
		if (bApplied)
		{
			Notify(Env, "CSV 가져옴: " + PendingImportFile, false);
		}
		PendingImportRows.clear();
		PendingImportWarnings.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ---------------------------------------------------------------- 그리기

void FDataTableEditor::DrawToolbar(FAssetEditorEnvironment& Env)
{
	const bool bHasStruct = Table.Struct != nullptr;
	const int32 SelectedIndex = Table.FindRowIndex(SelectedRow);

	ImGui::BeginDisabled(!bHasStruct);
	if (ImGui::Button(ICON_FA_PLUS " 행 추가"))
	{
		AddRow(SelectedIndex >= 0 ? SelectedIndex + 1 : -1);
	}
	ImGui::SetItemTooltip("선택한 행 뒤에 기본값 행을 추가합니다");
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedIndex < 0);
	if (ImGui::Button(ICON_FA_CLONE))
	{
		DuplicateSelected(Env);
	}
	ImGui::SetItemTooltip("복제 (Ctrl+D)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_TRASH_CAN))
	{
		DeleteRow(SelectedRow);
	}
	ImGui::SetItemTooltip("삭제 (Delete)");
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedIndex <= 0);
	if (ImGui::ArrowButton("##RowUp", ImGuiDir_Up))
	{
		MoveRow(SelectedIndex, SelectedIndex - 1);
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("위로 (파일 순서)");
	ImGui::SameLine();
	ImGui::BeginDisabled(SelectedIndex < 0 || SelectedIndex + 1 >= Table.GetRowCount());
	if (ImGui::ArrowButton("##RowDown", ImGuiDir_Down))
	{
		MoveRow(SelectedIndex, SelectedIndex + 1);
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("아래로 (파일 순서)");
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
	DataValueWidgets::InputString("##Filter", Filter);
	if (Filter.empty() && !ImGui::IsItemActive())
	{
		// 힌트 (InputTextWithHint는 std::string 크기 콜백과 함께 쓰기 번거로워 직접 그린다)
		const ImVec2 Min = ImGui::GetItemRectMin();
		ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + ImGui::GetStyle().FramePadding.x, Min.y + ImGui::GetStyle().FramePadding.y),
		                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_MAGNIFYING_GLASS " 행 이름/값 검색");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(SortColumn < 0);
	if (ImGui::Button(ICON_FA_ARROW_DOWN_WIDE_SHORT " 정렬 적용"))
	{
		ApplySortOrder();
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("머리글 정렬은 보기만 바꿉니다. 이 버튼은 현재 정렬 순서를 파일 행 순서로 저장합니다");
	ImGui::SameLine();
	ImGui::BeginDisabled(!bHasStruct);
	if (ImGui::Button(ICON_FA_FILE_EXPORT " CSV 내보내기"))
	{
		ExportCsv(Env);
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_FILE_IMPORT " CSV 가져오기"))
	{
		BeginImportCsv(Env);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("행 %d개", Table.GetRowCount());
	if (!Filter.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(보이는 %d)", static_cast<int32>(View.size()));
	}
	const size_t ProblemCount = LoadWarnings.size() + ReferenceWarnings.size();
	if (ProblemCount > 0)
	{
		ImGui::SameLine();
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " %d", static_cast<int32>(ProblemCount));
		ImGui::SetItemTooltip("검증 경고 (오른쪽 검증 항목)");
	}

	// 단축키 (창이 포커스이고 글자 입력 중이 아닐 때)
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && SelectedIndex >= 0 &&
	    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_F2))
		{
			RenamingRow  = SelectedRow;
			bFocusRename = true;
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			DeleteRow(SelectedRow);
		}
		else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
		{
			DuplicateSelected(Env);
		}
	}
}

void FDataTableEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawExternalChangeBanner(Env);
	if (ValidatedRevision != Revision && !ImGui::IsAnyItemActive())
	{
		Revalidate();
	}
	DrawToolbar(Env);
	if (Table.Struct == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_CIRCLE_EXCLAMATION " 구조체를 찾을 수 없습니다: %s", Table.StructPath.c_str());
		FAssetEditorWidgets::Hint("오른쪽 '구조체'에서 다른 구조체를 고르면 이름이 같은 필드 값이 되살아납니다. 값은 저장 시 원문 그대로 보존됩니다.");
	}
	DrawGrid(Env);
	DrawImportPopup(Env);
}

void FDataTableEditor::DrawGrid(FAssetEditorEnvironment& Env)
{
	const FDataStruct* Struct     = Table.Struct.get();
	const int32        FieldCount = Struct != nullptr ? static_cast<int32>(Struct->Fields.size()) : 0;
	const int32        Columns    = std::min(1 + FieldCount, 512);
	const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
	                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_Hideable |
	                              ImGuiTableFlags_SizingFixedFit;
	if (!ImGui::BeginTable("##DataRows", Columns, Flags, ImGui::GetContentRegionAvail()))
	{
		return;
	}
	const float Scale = ImGui::GetFontSize() / 16.0f;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("행 이름", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 150.0f * Scale, DataTableView::NameColumn);
	for (int32 Index = 0; Index + 1 < Columns; ++Index)
	{
		const FDataField& Field = Struct->Fields[static_cast<size_t>(Index)];
		ImGui::TableSetupColumn(Field.Name.c_str(), ImGuiTableColumnFlags_WidthFixed, DataTableView::GetDefaultColumnWidth(Field) * Scale,
		                        static_cast<ImGuiID>(1 + Index));
	}
	if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs(); Specs != nullptr && Specs->SpecsDirty)
	{
		SortColumn      = Specs->SpecsCount > 0 ? static_cast<int32>(Specs->Specs[0].ColumnUserID) : DataTableView::NoSort;
		bSortDescending = Specs->SpecsCount > 0 && Specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
		Specs->SpecsDirty = false;
	}
	RebuildViewIfNeeded();

	// 머리글 (필드 타입/설명 툴팁)
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int32 Column = 0; Column < Columns; ++Column)
	{
		if (!ImGui::TableSetColumnIndex(Column))
		{
			continue;
		}
		ImGui::PushID(Column);
		ImGui::TableHeader(ImGui::TableGetColumnName(Column));
		if (Column > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			ImGui::SetTooltip("%s", DataValueWidgets::DescribeField(Struct->Fields[static_cast<size_t>(Column - 1)]).c_str());
		}
		ImGui::PopID();
	}

	std::vector<std::function<void()>> Deferred; // 순회가 끝난 뒤 적용할 구조 변경 (이름 바꾸기/삭제/이동)
	const std::vector<FDataRow>&       Rows = Table.GetRows();

	ImGuiListClipper Clipper;
	Clipper.Begin(static_cast<int>(View.size()));
	if (bScrollToSelected)
	{
		const int32 SelectedIndex = Table.FindRowIndex(SelectedRow);
		const auto  Found         = std::find(View.begin(), View.end(), SelectedIndex);
		if (Found != View.end())
		{
			Clipper.IncludeItemByIndex(static_cast<int>(Found - View.begin()));
		}
		else
		{
			bScrollToSelected = false;
		}
	}
	while (Clipper.Step())
	{
		for (int ViewIndex = Clipper.DisplayStart; ViewIndex < Clipper.DisplayEnd; ++ViewIndex)
		{
			const int32 RowIndex = View[static_cast<size_t>(ViewIndex)];
			if (RowIndex < 0 || RowIndex >= static_cast<int32>(Rows.size()))
			{
				continue;
			}
			const FDataRow& Row       = Rows[static_cast<size_t>(RowIndex)];
			const bool      bSelected = Row.Name == SelectedRow;
			ImGui::PushID(RowIndex);
			ImGui::TableNextRow();
			if (bSelected)
			{
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
			}

			// 행 이름 칸
			ImGui::TableSetColumnIndex(0);
			if (bSelected && bScrollToSelected)
			{
				ImGui::SetScrollHereY(0.5f);
				bScrollToSelected = false;
			}
			if (RenamingRow == Row.Name)
			{
				if (bFocusRename)
				{
					ImGui::SetKeyboardFocusHere();
				}
				std::string NewName;
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (DataValueWidgets::InputTextCommit("##Rename", Row.Name, NewName))
				{
					const std::string From = Row.Name;
					Deferred.push_back([this, &Env, From, NewName] { RenameRow(Env, From, NewName); });
				}
				if (ImGui::IsItemActive())
				{
					bFocusRename = false;
				}
				else if (!bFocusRename)
				{
					RenamingRow.clear();
				}
			}
			else
			{
				ImGui::AlignTextToFramePadding();
				if (ImGui::Selectable(Row.Name.c_str(), bSelected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
				{
					SelectedRow = Row.Name;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						RenamingRow  = Row.Name;
						bFocusRename = true;
					}
				}
				if (ImGui::BeginPopupContextItem("##RowMenu"))
				{
					SelectedRow                 = Row.Name;
					const std::string RowName   = Row.Name;
					const int32       Index     = RowIndex;
					if (ImGui::MenuItem(ICON_FA_I_CURSOR " 이름 바꾸기", "F2"))
					{
						RenamingRow  = RowName;
						bFocusRename = true;
					}
					if (ImGui::MenuItem(ICON_FA_CLONE " 복제", "Ctrl+D"))
					{
						Deferred.push_back([this, &Env] { DuplicateSelected(Env); });
					}
					if (ImGui::MenuItem(ICON_FA_PLUS " 아래에 새 행"))
					{
						Deferred.push_back([this, Index] { AddRow(Index + 1); });
					}
					if (ImGui::MenuItem(ICON_FA_ARROW_UP " 위로", nullptr, false, Index > 0))
					{
						Deferred.push_back([this, Index] { MoveRow(Index, Index - 1); });
					}
					if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " 아래로", nullptr, false, Index + 1 < Table.GetRowCount()))
					{
						Deferred.push_back([this, Index] { MoveRow(Index, Index + 1); });
					}
					if (ImGui::MenuItem(ICON_FA_COPY " 이름 복사"))
					{
						ImGui::SetClipboardText(RowName.c_str());
					}
					ImGui::Separator();
					if (ImGui::MenuItem(ICON_FA_TRASH_CAN " 삭제", "Delete"))
					{
						Deferred.push_back([this, RowName] { DeleteRow(RowName); });
					}
					ImGui::EndPopup();
				}
			}

			// 값 칸
			for (int32 FieldIndex = 0; FieldIndex + 1 < Columns; ++FieldIndex)
			{
				if (!ImGui::TableSetColumnIndex(1 + FieldIndex) || static_cast<size_t>(FieldIndex) >= Row.Record.Values.size())
				{
					continue;
				}
				const FDataField& Field = Struct->Fields[static_cast<size_t>(FieldIndex)];
				ImGui::PushID(FieldIndex);
				ImGui::PushItemWidth(-FLT_MIN);
				FDataValue         Value = Row.Record.Values[static_cast<size_t>(FieldIndex)];
				FDataWidgetContext Context;
				Context.Editor    = Env.Editor;
				Context.SelfPath  = Path;
				Context.SelfTable = &Table;
				Context.Problem   = FindCellProblem(RowIndex, FieldIndex);
				Context.bCompact  = true;
				if (bSelected && !AutoPopupField.empty() && AutoPopupField == Field.Name)
				{
					Context.bOpenPopup = true;
					AutoPopupField.clear();
				}
				ImGui::BeginGroup();
				const bool bChanged = DataValueWidgets::Draw(Field, Value, Context);
				ImGui::EndGroup();
				if (ImGui::IsItemActive())
				{
					SelectedRow = Row.Name;
				}
				if (bChanged && Table.SetValue(Row.Name, Field.Name, Value))
				{
					Edited("값 편집");
				}
				ImGui::PopItemWidth();
				ImGui::PopID();
			}
			ImGui::PopID();
		}
	}
	ImGui::EndTable();

	for (const std::function<void()>& Action : Deferred)
	{
		Action();
	}
}

void FDataTableEditor::DrawStructSection(FAssetEditorEnvironment& Env)
{
	if (!ImGui::CollapsingHeader(ICON_FA_TABLE_LIST " 구조체", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	ImGui::TextUnformatted(Table.StructPath.empty() ? "(없음)" : Table.StructPath.c_str());
	if (Table.Struct != nullptr)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_PEN_TO_SQUARE " 구조체 편집") && Env.Editor != nullptr && Env.Editor->OpenAssetEditorRequest)
		{
			Env.Editor->OpenAssetEditorRequest(FDataLibrary::Get().ResolvePath(Table.StructPath));
		}
		if (!Table.Struct->Description.empty())
		{
			FAssetEditorWidgets::Hint(Table.Struct->Description.c_str());
		}
		ImGui::TextDisabled("필드 %d개", static_cast<int32>(Table.Struct->Fields.size()));
	}
	else
	{
		ImGui::TextColored(FEditorTheme::Danger, "구조체를 찾을 수 없습니다");
	}
	if (Env.Editor != nullptr && ImGui::BeginCombo("구조체 바꾸기", Table.StructPath.c_str()))
	{
		for (const std::string& StructPath : DataValueWidgets::ScanContentFiles(Env.Editor->ContentDirectory, FDataStruct::Extension))
		{
			if (ImGui::Selectable(StructPath.c_str(), StructPath == Table.StructPath))
			{
				ChangeStruct(StructPath);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("이름이 같은 필드 값은 옮기고, 없는 필드 값은 원문으로 보존합니다");
}

void FDataTableEditor::DrawSelectedRow(FAssetEditorEnvironment& Env)
{
	if (!ImGui::CollapsingHeader(ICON_FA_LIST " 선택한 행", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	const int32 RowIndex = Table.FindRowIndex(SelectedRow);
	if (RowIndex < 0 || Table.Struct == nullptr)
	{
		ImGui::TextDisabled("표에서 행을 고르세요");
		return;
	}
	std::string NewName;
	if (DataValueWidgets::InputTextCommit("이름", SelectedRow, NewName))
	{
		RenameRow(Env, SelectedRow, NewName);
		return; // 다음 프레임에 새 이름으로
	}
	ImGui::SetItemTooltip("행 이름 (이 테이블 안의 자기 참조는 함께 바뀝니다)");

	const FDataRow& Row = Table.GetRows()[static_cast<size_t>(RowIndex)];
	if (!ImGui::BeginTable("##RowFields", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
	{
		return;
	}
	ImGui::TableSetupColumn("필드", ImGuiTableColumnFlags_WidthStretch, 0.35f);
	ImGui::TableSetupColumn("값", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
	const std::vector<FDataField>& Fields = Table.Struct->Fields;
	std::string                    RowName = Row.Name; // SetValue 뒤에도 안전하게
	for (size_t FieldIndex = 0; FieldIndex < Fields.size() && FieldIndex < Row.Record.Values.size(); ++FieldIndex)
	{
		const FDataField& Field = Fields[FieldIndex];
		ImGui::PushID(static_cast<int>(FieldIndex));
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(Field.Name.c_str());
		ImGui::SetItemTooltip("%s", DataValueWidgets::DescribeField(Field).c_str());
		ImGui::TableNextColumn();
		ImGui::PushItemWidth(-FLT_MIN);
		FDataValue         Value = Row.Record.Values[FieldIndex];
		FDataWidgetContext Context;
		Context.Editor    = Env.Editor;
		Context.SelfPath  = Path;
		Context.SelfTable = &Table;
		Context.Problem   = FindCellProblem(RowIndex, static_cast<int32>(FieldIndex));
		const bool bChanged = DataValueWidgets::Draw(Field, Value, Context);
		ImGui::PopItemWidth();
		ImGui::TableNextColumn();
		const bool bIsDefault = Value == Field.Default;
		ImGui::BeginDisabled(bIsDefault);
		const bool bReset = ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT);
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("구조체 기본값으로");
		if (bReset)
		{
			Value = Field.Default;
		}
		if ((bChanged || bReset) && Table.SetValue(RowName, Field.Name, Value))
		{
			Edited(bReset ? "기본값으로" : "값 편집");
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void FDataTableEditor::DrawValidation()
{
	const size_t Count = LoadWarnings.size() + ReferenceWarnings.size();
	const std::string Header = Count == 0 ? std::string(ICON_FA_CIRCLE_CHECK " 검증###Validation")
	                                      : std::format(ICON_FA_TRIANGLE_EXCLAMATION " 검증 ({})###Validation", Count);
	if (!ImGui::CollapsingHeader(Header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	if (Count == 0)
	{
		ImGui::TextColored(FEditorTheme::Success, "문제 없음 (행 참조·에셋 경로 확인됨)");
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Warning);
	for (const std::string& Warning : LoadWarnings)
	{
		ImGui::TextWrapped("%s", Warning.c_str());
	}
	for (size_t Index = 0; Index < ReferenceWarnings.size(); ++Index)
	{
		const std::string& Warning = ReferenceWarnings[Index];
		ImGui::PushID(static_cast<int>(Index));
		const std::string RowName = ExtractRowName(Warning);
		if (!RowName.empty() && Table.HasRow(RowName))
		{
			// 누르면 그 행으로
			if (ImGui::Selectable(Warning.c_str(), RowName == SelectedRow))
			{
				SelectedRow       = RowName;
				bScrollToSelected = true;
			}
			ImGui::SetItemTooltip("%s\n(눌러서 행 선택)", Warning.c_str());
		}
		else
		{
			ImGui::TextWrapped("%s", Warning.c_str());
		}
		ImGui::PopID();
	}
	ImGui::PopStyleColor();
}

void FDataTableEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	DrawStructSection(Env);
	DrawSelectedRow(Env);
	DrawValidation();
}

// ---------------------------------------------------------------- 자동 검증

void FDataTableEditor::RunVerifyRoundTrip(FAssetEditorEnvironment& Env)
{
	std::vector<std::string> Failures;
	const auto Check = [&](bool bOk, const char* What) {
		if (!bOk)
		{
			Failures.emplace_back(What);
		}
	};
	const std::string Original = CaptureState();

	// 1) 편집: 행 추가 → 첫 Float/Int 값 → 복제 → 이름 바꾸기 → 실행 취소/다시 실행
	AddRow(-1);
	CommitPendingEdit(false);
	const std::string Added = SelectedRow;
	if (Table.Struct != nullptr)
	{
		for (const FDataField& Field : Table.Struct->Fields)
		{
			if (Field.Type == EDataFieldType::Float || Field.Type == EDataFieldType::Int)
			{
				const FDataValue Value = Field.Type == EDataFieldType::Float ? FDataValue::MakeFloat(123.5f) : FDataValue::MakeInt(77);
				Check(Table.SetValue(Added, Field.Name, Value), "값 설정");
				Edited("값 편집");
				CommitPendingEdit(false);
				break;
			}
		}
	}
	DuplicateSelected(Env);
	CommitPendingEdit(false);
	Check(RenameRow(Env, SelectedRow, "VerifyRenamed"), "이름 바꾸기");
	CommitPendingEdit(false);
	const std::string EditedState = CaptureState();
	Undo(Env);
	Check(!Table.HasRow("VerifyRenamed"), "실행 취소");
	Redo(Env);
	Check(CaptureState() == EditedState, "다시 실행");
	Check(Table.HasRow(Added) && Table.HasRow("VerifyRenamed"), "행 추가/복제");

	// 2) 저장 → 다시 읽기 (임시 파일 — 원본은 건드리지 않음)
	const std::filesystem::path VerifyDirectory = FPaths::GetSavedDirectory() / "Verify";
	const std::filesystem::path TempTable       = VerifyDirectory / ("DataRoundTrip" + Path.extension().string());
	const std::string           TempPath        = FStringConv::ToUtf8(TempTable.generic_wstring());
	std::string                 Error;
	Check(FDataLibrary::Get().SaveTable(TempPath, Table, &Error), "임시 저장");
	const std::shared_ptr<const FDataTable> Reloaded = FDataLibrary::Get().LoadTable(TempPath);
	Check(Reloaded != nullptr && Reloaded->ToJsonString() == Table.ToJsonString(), "저장 후 다시 읽기 일치");

	// 3) CSV 왕복 (내보내기 → 가져오기 → 같은 내용)
	const std::filesystem::path TempCsv = VerifyDirectory / "DataRoundTrip.csv";
	std::vector<FDataRow>       CsvRows;
	std::vector<std::string>    CsvWarnings;
	Check(ExportCsvTo(TempCsv, &Error), "CSV 내보내기");
	Check(ReadCsvRows(TempCsv, CsvRows, CsvWarnings, &Error), "CSV 가져오기");
	FDataTable CsvTable = Table;
	CsvTable.SetRows(CsvRows);
	Check(CsvTable.ToJsonString() == Table.ToJsonString() && CsvWarnings.empty(), "CSV 왕복 일치");
	std::error_code ErrorCode;
	std::filesystem::remove(TempTable, ErrorCode);
	std::filesystem::remove(TempCsv, ErrorCode);

	// 4) 되돌리기 → 원래 파일 상태
	RevertToSaved(Env);
	Check(CaptureState() == Original && !IsDirty(), "되돌리기");

	if (Failures.empty())
	{
		E_LOG(LogEditor, Display, "[데이터 편집 검증] 성공: {} (편집/Undo/저장·다시 읽기/CSV 왕복)", GetDisplayName());
	}
	else
	{
		std::string Joined;
		for (const std::string& Failure : Failures)
		{
			Joined += (Joined.empty() ? "" : ", ") + Failure;
		}
		E_LOG(LogEditor, Error, "[데이터 편집 검증] 실패: {} — {} {}", GetDisplayName(), Joined, Error);
	}
}

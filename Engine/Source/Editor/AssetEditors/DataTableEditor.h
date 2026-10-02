#pragma once

#include "Editor/AssetEditors/DataEditorBase.h"
#include "Scene/DataTable.h"

#include <string>
#include <unordered_map>
#include <vector>

// .etable 데이터 테이블 편집 창 (언리얼 DataTable 편집기식):
//   왼쪽 = 스프레드시트 (행 이름 열·머리글 고정, 열 폭 조절/숨기기, 필드 타입별 칸 위젯, 검색, 머리글 눌러 보기 정렬 — 파일 순서는 "정렬 적용"만 바꾼다)
//   오른쪽 = 구조체 정보(열기/바꾸기), 선택한 행 인스펙터(설명 + 기본값으로), 검증(읽기 경고 + 참조 경고 — 칸에도 경고 색/툴팁)
//   행: 추가/복제/삭제/이름 바꾸기(더블클릭 또는 F2, 이 테이블 안 자기 참조 RowRef도 함께)/위·아래 이동. CSV 내보내기/가져오기(교체·병합)
// 자동 검증 인자: --data-select <행 이름>, --data-popup <필드 이름>(선택 행의 배열/글자 팝업), --verify-data-roundtrip(편집→저장→다시 읽기, CSV 왕복)
class FDataTableEditor final : public FDataEditorBase
{
public:
	using FDataEditorBase::FDataEditorBase;

	const char* GetTypeName() const override { return "데이터 테이블"; }
	float       GetPropertiesWidthWeight() const override { return 0.7f; }
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        OnDataLibraryChanged(FAssetEditorEnvironment& Env) override;

private:
	void DrawToolbar(FAssetEditorEnvironment& Env);
	void DrawGrid(FAssetEditorEnvironment& Env);
	void DrawRowNameCell(FAssetEditorEnvironment& Env, const FDataRow& Row, int32 RowIndex);
	void DrawRowContextMenu(FAssetEditorEnvironment& Env, const std::string& RowName, int32 RowIndex);
	void DrawSelectedRow(FAssetEditorEnvironment& Env);
	void DrawStructSection(FAssetEditorEnvironment& Env);
	void DrawValidation();
	void DrawImportPopup(FAssetEditorEnvironment& Env);

	// 편집 (성공하면 MarkEdited + 보기/검증 갱신)
	void Edited(std::string_view Label);
	void AddRow(int32 Index);
	void DuplicateSelected(FAssetEditorEnvironment& Env);
	void DeleteRow(const std::string& RowName);
	bool RenameRow(FAssetEditorEnvironment& Env, const std::string& From, const std::string& To);
	void MoveRow(int32 From, int32 To);
	void ChangeStruct(const std::string& StructPath);
	void ApplySortOrder();

	void ExportCsv(FAssetEditorEnvironment& Env);
	void BeginImportCsv(FAssetEditorEnvironment& Env);
	bool ExportCsvTo(const std::filesystem::path& File, std::string* OutError) const;
	bool ReadCsvRows(const std::filesystem::path& File, std::vector<FDataRow>& OutRows, std::vector<std::string>& OutWarnings, std::string* OutError) const;

	void Revalidate();
	void RebuildViewIfNeeded();
	const std::string* FindCellProblem(int32 RowIndex, int32 FieldIndex) const;
	std::vector<int32> GetSelfReferenceFields() const;
	void RunVerifyRoundTrip(FAssetEditorEnvironment& Env);

	FDataTable               Table;
	std::vector<std::string> LoadWarnings; // 파일 읽기 경고 (구조체 없음, 타입 불일치 등)

	// 보기 (파일 순서 불변)
	std::vector<int32> View;
	std::string        Filter;
	int32              SortColumn  = -1;
	bool               bSortDescending = false;
	uint64             Revision     = 1; // 테이블 내용이 바뀔 때마다
	uint64             ViewRevision = 0;
	std::string        ViewFilter;
	int32              ViewSortColumn = -2;
	bool               bViewDescending = false;

	// 검증 (편집이 끝났을 때 다시)
	uint64                                 ValidatedRevision = 0;
	std::unordered_map<uint64, std::string> CellProblems; // (행 << 32) | 필드
	std::vector<std::string>               ReferenceWarnings;

	// 선택 / 이름 바꾸기
	std::string SelectedRow;
	std::string RenamingRow;
	bool        bFocusRename      = false;
	bool        bScrollToSelected = false;

	// CSV 가져오기 대기 (교체/병합 선택)
	std::vector<FDataRow>    PendingImportRows;
	std::vector<std::string> PendingImportWarnings;
	std::string              PendingImportFile;
	bool                     bOpenImportPopup = false;

	// 자동 검증
	std::string AutoPopupField;
	int32       FrameCounter   = 0;
	bool        bAutoArgsRead  = false;
};

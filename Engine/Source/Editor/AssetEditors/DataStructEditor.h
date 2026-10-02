#pragma once

#include "Editor/AssetEditors/DataEditorBase.h"
#include "Scene/DataTable.h"

#include <filesystem>
#include <string>
#include <vector>

// .estruct 데이터 구조체 편집 창: 왼쪽 = 필드 목록(이름/타입/기본값/설명 바로 편집, 추가/복제/삭제/순서),
// 오른쪽 = 구조체 이름·설명, 선택한 필드 상세(배열 요소 타입, Enum 값 목록, 에셋 필터, RowRef 대상 테이블, 기본값), 사용 파일, 대기 중 이름 변경.
// 저장 = FDataLibrary::SaveStructAndMigrate(필드 이름 변경 누적 목록) — 이 구조체를 쓰는 .etable/.edata가 있으면 확인 후 함께 다시 쓴다.
// 실행 취소 상태 = 구조체 JSON + 이름 변경 목록. 저장하면 이름 변경이 적용됐으므로 실행 취소 기록을 새로 시작한다
class FDataStructEditor final : public FDataEditorBase
{
public:
	using FDataEditorBase::FDataEditorBase;

	const char* GetTypeName() const override { return "데이터 구조체"; }
	float       GetPropertiesWidthWeight() const override { return 0.9f; }

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;

private:
	void DrawFieldList(FAssetEditorEnvironment& Env);
	void DrawFieldDetails(FAssetEditorEnvironment& Env);
	void DrawMigrateConfirm(FAssetEditorEnvironment& Env);
	bool DrawTypeCombo(const char* Id, EDataFieldType& InOutType, bool bElement);

	// 편집 (실패하면 FieldError에 이유)
	bool ApplyField(int32 Index, FDataField Field, std::string_view Label);
	bool RenameField(int32 Index, const std::string& NewName);
	void ChangeType(int32 Index, EDataFieldType Type, EDataFieldType ElementType);
	void AddField();
	void DuplicateField(int32 Index);
	void RemoveField(int32 Index);
	void MoveField(int32 From, int32 To);
	void RefreshUsers(FAssetEditorEnvironment& Env);

	FDataStruct                        Struct;
	std::vector<FDataFieldRename>      Renames; // 마지막 저장 뒤 필드 이름 변경 (순서대로)
	std::vector<std::filesystem::path> Users;   // 이 구조체를 쓰는 .etable/.edata
	std::vector<std::string>           LoadWarnings;
	std::string                        FieldError;
	int32                              SelectedField       = -1;
	bool                               bOpenMigrateConfirm = false;
	bool                               bMigrationConfirmed = false;
	bool                               bAutoArgsRead       = false;
	bool                               bAutoPopup          = false; // --data-popup (선택 필드 기본값 팝업)
};

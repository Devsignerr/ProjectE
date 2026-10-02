#pragma once

#include "Editor/AssetEditors/DataEditorBase.h"
#include "Scene/DataTable.h"

#include <string>
#include <unordered_map>
#include <vector>

// .edata 데이터 에셋 편집 창: 왼쪽 = 인스펙터식 필드 목록(테이블 칸과 같은 위젯, 배열은 펼쳐서, 필드마다 기본값으로),
// 오른쪽 = 구조체 정보(편집/바꾸기) + 검증(읽기 경고 + 참조 경고 — 칸에도 경고 색/툴팁)
class FDataAssetEditor final : public FDataEditorBase
{
public:
	using FDataEditorBase::FDataEditorBase;

	const char* GetTypeName() const override { return "데이터 에셋"; }
	float       GetPropertiesWidthWeight() const override { return 0.8f; }

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        OnDataLibraryChanged(FAssetEditorEnvironment& Env) override;

private:
	void Edited(std::string_view Label);
	void Revalidate();

	FDataAsset                             Asset;
	std::vector<std::string>               LoadWarnings;
	std::vector<std::string>               ReferenceWarnings;
	std::unordered_map<int32, std::string> FieldProblems;
	uint64                                 Revision          = 1;
	uint64                                 ValidatedRevision = 0;
};

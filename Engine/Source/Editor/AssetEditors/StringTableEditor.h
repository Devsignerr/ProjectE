#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "UI/Localization.h"

#include <string>

// .estrings 문자열 표 편집 창: 왼쪽 = 키 × 언어 표 (칸을 바로 고친다, 검색/번역 빠진 것만), 오른쪽 = 언어 목록·키 추가/이름 바꾸기/삭제.
// 저장하면 FLocalization::Reload → 열린 UI(디자이너 미리보기, 플레이 중 화면)가 바로 새 문자열로 바뀐다.
class FStringTableEditor final : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	const char* GetTypeName() const override { return "문자열 표"; }
	bool        UsesPreview() const override { return false; }
	float       GetPropertiesWidthWeight() const override { return 0.8f; }

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;

private:
	bool IsMissingTranslation(const std::string& Key) const;

	FStringTable Table;
	std::string  SelectedKey;
	char         Filter[128]      = {};
	char         NewKey[128]      = {};
	char         RenameBuffer[128] = {};
	char         NewLanguage[32]  = {};
	bool         bOnlyMissing     = false;
	std::string  PendingDeleteKey;
	std::string  PendingDeleteLanguage;
};

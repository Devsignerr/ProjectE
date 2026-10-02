#pragma once

#include "Editor/AssetEditors/AssetEditor.h"

#include <filesystem>
#include <string>

// 데이터 편집기(.etable/.edata/.estruct) 공통: 디스크 변경 감시(핫 리로드)와 데이터 캐시 세대 추적.
//   - 파일 쓰기 시각이 바뀌면: 저장 안 한 변경이 없으면 바로 다시 읽고, 있으면 알림 띠를 띄워 사용자가 고른다(다시 읽기 / 내 변경 유지)
//   - FDataLibrary 세대가 바뀌면(구조체·참조 테이블 저장 등) OnDataLibraryChanged — 테이블/에셋은 구조체를 다시 묶고 검증을 새로 한다
class FDataEditorBase : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	bool UsesPreview() const override { return false; }
	void Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	// 파일을 읽거나 쓴 직후 (자기 저장을 외부 변경으로 보지 않게)
	void        RememberDiskState();
	std::string GetAssetPathString() const; // 절대 경로 UTF-8 (FDataLibrary 경로 인자)
	// 외부 변경 알림 띠 (DrawPreviewArea 맨 위에서 호출)
	void        DrawExternalChangeBanner(FAssetEditorEnvironment& Env);
	void        Notify(FAssetEditorEnvironment& Env, const std::string& Message, bool bError) const;
	virtual void OnDataLibraryChanged(FAssetEditorEnvironment& Env) { (void)Env; }

private:
	std::filesystem::file_time_type KnownWriteTime{};
	uint32                          KnownGeneration = 0;
	float                           PollTimer       = 0.0f;
	bool                            bExternalChange = false;
};

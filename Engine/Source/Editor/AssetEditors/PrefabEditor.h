#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Editor/Panels/InspectorPanel.h"

#include <filesystem>
#include <string>
#include <vector>

// .eprefab 프리팹 편집 창: 미리보기 씬에 원본을 펼쳐 놓고 계층(선택/추가/삭제/중첩 프리팹 드롭) + 인스펙터로 고친다.
// 편집은 창 안에서만 (열린 씬 인스턴스는 그대로), 저장하면 FEditorContext::ChangePrefab을 거쳐 열린 씬 인스턴스가 새 원본에 맞춰진다.
// 저장 후에는 새 엔티티에 부여된 ID를 기록 기준으로 삼기 위해 창 안 실행 취소 기록을 새로 시작한다.
class FPrefabEditor final : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	const char* GetTypeName() const override { return "프리팹"; }
	float       GetPropertiesWidthWeight() const override { return 1.3f; }

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;

private:
	void DrawNode(FAssetEditorEnvironment& Env, FEntity Entity);
	void DrawNodeMenu(FAssetEditorEnvironment& Env, FEntity Entity);
	void ApplyPendingChanges(FAssetEditorEnvironment& Env);
	void AddNestedPrefab(FAssetEditorEnvironment& Env, const std::filesystem::path& File, FEntity Parent);
	void ResolvePreviewAssets(FAssetEditorEnvironment& Env);
	void DestroyContent();

	FEntity         Root;       // 미리보기 씬 안 프리팹 루트 (기본 조명은 별도 루트)
	uint32          NextId = 1; // 파일의 NextId (새 엔티티 ID는 저장 시 부여)
	FEntity         Selected;
	FInspectorPanel Inspector;

	// 트리 순회가 끝난 뒤 적용할 구조 변경
	FEntity                            PendingDelete;
	FEntity                            PendingDuplicate;
	FEntity                            PendingAddParent;
	std::string                        PendingAddKind; // "empty" / "cube"
	std::vector<std::filesystem::path> PendingPrefabDrops;
	FEntity                            PendingDropParent;
};

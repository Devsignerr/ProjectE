#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/ModelImportSettings.h"

#include <string>
#include <vector>

struct FModelResources;

// glTF 모델(.glb/.gltf) 미리보기 공통: 모델 배치, 정보(노드/메시/머티리얼/경계), 와이어프레임, 씬에 추가.
// 모델 파일 자체는 편집하지 않으므로 저장할 상태가 없다 (임포트 설정은 후속 과제 — Plans.md Phase 9 결정 참고)
class FModelEditorBase : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	bool HasEditableState() const override { return false; }

	// 애니메이션이 있으면 애니메이션 편집기로 연다
	static bool HasAnimations(const std::filesystem::path& Path, FResourceManager& Resources);

	// 다시 가져온 뒤: 미리보기 모델을 새 리소스로 다시 배치
	virtual void RebuildPreview(FAssetEditorEnvironment& Env);

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override { return {}; }
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;
	void        RenderPreview(FAssetEditorEnvironment& Env) override;

	void DrawModelInfo(FAssetEditorEnvironment& Env);
	void DrawAddToScene(FAssetEditorEnvironment& Env);
	// 임포트 설정 (원본 옆 .eimport) 편집 + 다시 가져오기
	void DrawImportSettings(FAssetEditorEnvironment& Env);

	const FModelResources* Model     = nullptr; // FResourceManager 모델 캐시 (주소 고정)
	FEntity                ModelRoot;
	bool                   bWireframe = false;
	FModelImportSettings   ImportSettings;      // 편집 중 값
	FModelImportSettings   SavedImportSettings; // 파일에 있는 값 (바뀌었는지 비교)
	bool                   bImportSettingsLoaded = false;
};

class FStaticMeshEditor final : public FModelEditorBase
{
public:
	using FModelEditorBase::FModelEditorBase;
	const char* GetTypeName() const override { return "스태틱 메시"; }

protected:
	void DrawProperties(FAssetEditorEnvironment& Env) override;
};

// 스킨/애니메이션 모델: 클립 선택, 재생/일시정지/한 프레임, 타임라인 스크럽, 속도·루프, 뼈대 표시
class FAnimationEditor final : public FModelEditorBase
{
public:
	using FModelEditorBase::FModelEditorBase;
	const char* GetTypeName() const override { return "애니메이션"; }

	void Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	bool LoadAsset(FAssetEditorEnvironment& Env) override;
	void DrawProperties(FAssetEditorEnvironment& Env) override;
	void DrawPreviewOverlay(FAssetEditorEnvironment& Env) override;
	void DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;

private:
	void DrawBones();

	std::vector<std::string> ClipNames;
	std::vector<FEntity>     Joints; // 스킨 관절 엔티티 (뼈대 표시)
	bool                     bShowBones = true;
	float                    StepSeconds = 1.0f / 30.0f;
};

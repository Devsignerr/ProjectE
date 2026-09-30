#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/MaterialAsset.h"
#include "Scene/ResourceHandles.h"

#include <string>
#include <vector>

// .emat 머티리얼 편집기. 씬과 같은 머티리얼 리소스(경로 캐시)를 직접 고치므로 열린 씬에 즉시 반영되고,
// 저장하지 않고 닫으면 파일 상태로 되돌린다.
class FMaterialEditor final : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	const char* GetTypeName() const override { return "머티리얼"; }

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;

private:
	// 편집 값 → 공유 머티리얼 (텍스처가 바뀌었으면 디스크립터 테이블도 다시 만든다)
	void ApplyToMaterial(FAssetEditorEnvironment& Env, bool bTexturesChanged);
	void SetPreviewShape(FAssetEditorEnvironment& Env, const char* PrimitiveName);
	void ScanTextureFiles(const std::filesystem::path& ContentDirectory);
	bool DrawTextureSlot(FAssetEditorEnvironment& Env, uint32 Slot, const char* Label);

	FMaterialAsset           Asset;
	FMaterialHandle          Material;
	FEntity                  PreviewEntity;
	std::string              PreviewShape = "sphere";
	std::vector<std::string> TextureFiles; // .emat 폴더 기준 상대 경로 ('/' 구분)
};

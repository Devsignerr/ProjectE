#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/MaterialAsset.h"
#include "Scene/ResourceHandles.h"

#include <string>
#include <vector>

// .emat 머티리얼 편집기. 씬과 같은 머티리얼 리소스(경로 캐시)를 직접 고치므로 열린 씬에 즉시 반영되고,
// 저장하지 않고 닫으면 파일 상태로 되돌린다.
// 머티리얼 인스턴스(부모 지정)는 항목마다 "덮어쓰기" 체크로 부모 값을 따를지 고르고, 덮어쓰지 않은 항목은 부모 값을 흐리게 보여 준다.
class FMaterialEditor final : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	const char* GetTypeName() const override { return "머티리얼"; }
	void        CollectResourceRoots(FResourceRoots& Roots) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;

private:
	// 편집 값 → 공유 머티리얼 (리소스 관리자가 부모 체인을 해석하고, 텍스처가 바뀐 경우에만 디스크립터 테이블을 다시 만든다)
	void ApplyToMaterial(FAssetEditorEnvironment& Env);
	// 부모에게서 물려받는 값(Inherited)과 해석 오류를 다시 계산한다
	void RefreshInherited(FAssetEditorEnvironment& Env);
	void SetPreviewShape(FAssetEditorEnvironment& Env, const char* PrimitiveName);
	void ScanFiles(const std::filesystem::path& ContentDirectory);
	bool DrawTextureSlot(FAssetEditorEnvironment& Env, uint32 Slot, const char* Label);
	// 인스턴스면 "덮어쓰기" 체크박스를 그린다. 체크를 끄면 그 항목은 부모 값으로 돌아간다. 반환: 체크가 바뀌었으면 true
	bool DrawOverrideToggle(uint32 Field);
	bool DrawParent(FAssetEditorEnvironment& Env);
	// 그래프 머티리얼(Phase 49 사이드): 컴파일 상태 + 파라미터 값(인스턴스는 덮어쓰기 체크) + 생성 HLSL 보기. 노드 편집 UI는 Phase 51
	// 반환: 값이 바뀌었으면 true (bOutTextures = 텍스처 경로가 바뀜)
	bool DrawGraphSection(FAssetEditorEnvironment& Env, bool& bOutTextures);
	void RefreshGraphStatus(FAssetEditorEnvironment& Env);

	FMaterialAsset           Asset;
	FMaterialAsset           Inherited; // 인스턴스: 부모 체인만 해석한 값 (덮어쓰지 않은 항목에 보여 준다)
	std::string              ParentError;
	bool                     bGraphMaterial = false; // 해석 결과가 그래프 머티리얼 (인스턴스는 부모 그래프)
	std::string              GraphErrors;           // 그래프 컴파일 오류 (비면 성공)
	std::string              GraphHlsl;             // 생성 HLSL (읽기 전용 보기)
	uint64                   GraphHash = 0;
	size_t                   GraphNodeCount = 0;
	std::vector<FMaterialParameter> ResolvedParameters; // 해석된 전체 파라미터 (인스턴스: 부모 값 + 덮어쓰기)
	FMaterialHandle          Material;
	FEntity                  PreviewEntity;
	std::string              PreviewShape = "sphere";
	std::vector<std::string> TextureFiles;  // .emat 폴더 기준 상대 경로 ('/' 구분)
	std::vector<std::string> MaterialFiles; // 부모 후보 .emat (같은 기준, 자기 자신 제외)
};

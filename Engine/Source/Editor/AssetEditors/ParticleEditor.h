#pragma once

#include "Editor/AssetEditors/AssetEditor.h"

#include <memory>
#include <string>
#include <vector>

struct FParticleEmitter;
struct FParticleSystemAsset;

// .eparticle 파티클 편집기 (나이아가라식): 이미터 목록 → 선택한 이미터의 단계별 모듈 목록(추가/삭제/순서/켜기) + 렌더러.
// 모듈 입력은 상수/무작위 범위/곡선으로 바꿀 수 있다. 리소스 관리자의 공유 에셋을 직접 고치므로 열린 씬에 즉시 반영되고,
// 저장하지 않고 닫으면 파일 상태로 되돌린다.
class FParticleEditor final : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	const char* GetTypeName() const override { return "파티클"; }
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;
	float       GetPropertiesWidthWeight() const override { return 1.5f; }

	// 이미터 템플릿 (편집기 "+ 이미터" 메뉴, 테스트). 이름 목록과 같은 순서
	static std::vector<const char*> GetTemplateNames();
	static FParticleEmitter         MakeTemplate(size_t Index);

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;
	void        FramePreview(FAssetEditorEnvironment& Env) override;

private:
	void RestartPreview();
	bool DrawEmitterList(bool& bOutStructure);
	bool DrawEmitterSettings(FParticleEmitter& Emitter, bool& bOutRestart);
	bool DrawModuleStacks(FParticleEmitter& Emitter, bool& bOutRestart);
	bool DrawRenderers(FParticleEmitter& Emitter, bool& bOutResolve);

	std::shared_ptr<FParticleSystemAsset> System; // 리소스 관리자 캐시와 공유
	FEntity                               EmitterEntity;
	std::vector<std::string>              TextureFiles;
	int32                                 SelectedEmitter = 0;
	bool                                  bPaused         = false;
};

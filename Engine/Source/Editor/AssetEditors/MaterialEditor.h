#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/MaterialAsset.h"
#include "Scene/ResourceHandles.h"

#include <memory>
#include <string>
#include <vector>

class FMaterialGraphPanel;

// .emat 머티리얼 편집기. 씬과 같은 머티리얼 리소스(경로 캐시)를 직접 고치므로 열린 씬에 즉시 반영되고,
// 저장하지 않고 닫으면 파일 상태로 되돌린다.
// 머티리얼 인스턴스(부모 지정)는 항목마다 "덮어쓰기" 체크로 부모 값을 따를지 고르고, 덮어쓰지 않은 항목은 부모 값을 흐리게 보여 준다.
//
// 그래프 머티리얼(Phase 51 사이드 — 언리얼 머티리얼 에디터식): 왼쪽 = 노드 그래프(FMaterialGraphPanel), 오른쪽 = 미리보기(구/큐브/평면) +
// 탭(상세: 선택 노드 설정 또는 머티리얼 설정 / 파라미터 / 생성 HLSL). 인스턴스는 부모 그래프를 읽기 전용으로 보이고 파라미터 덮어쓰기만 고친다.
//   - 편집마다 컴파일 + 분석(노드 오류 표시)은 즉시, 공유 머티리얼 반영은 셰이더 해시가 그대로면 즉시(값 조절) / 바뀌면 0.2초 뒤(DXC 디바운스).
//     컴파일 오류면 반영하지 않는다(렌더러는 이전 셰이더 유지)
//   - 위치 없는 노드(EditorPosition (0,0))는 열 때 자동 배치(MaterialGraphEditing::AutoLayout) — 상태에 포함되어 저장하면 파일에 들어간다
//   - 그래프 없는 일반 머티리얼은 "그래프로 변환"(같은 출력의 노드)
//   - 자동 검증: --matgraph-select <노드 Id>, --matgraph-tab details|params|hlsl, --matgraph-palette(팔레트 열기),
//     --matgraph-convert(그래프 없는 머티리얼을 열자마자 그래프로 변환, 저장 안 함),
//     --verify-matgraph-roundtrip(현재 상태 → JSON → 다시 읽기 → 생성 HLSL 해시·JSON 동일 확인, 로그 [머티리얼 그래프 왕복]),
//     --verify-matgraph-edit(노드 추가·연결 → 디바운스 뒤 공유 머티리얼 셰이더 변경 → 실행 취소로 복원, 로그 [머티리얼 그래프 편집 검증])
class FMaterialEditor final : public FAssetEditor
{
public:
	explicit FMaterialEditor(std::filesystem::path InPath);
	~FMaterialEditor() override;

	const char* GetTypeName() const override { return "머티리얼"; }
	void        CollectResourceRoots(FResourceRoots& Roots) override;
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;
	float       GetPropertiesWidthWeight() const override { return bGraphMaterial ? 0.9f : 1.0f; }

	// 왕복 검증 (자동 검증·테스트): 현재 편집 상태를 저장 형식으로 썼다 다시 읽어 같은 셰이더/같은 JSON인지. 반환: 성공
	bool VerifyRoundTrip(FAssetEditorEnvironment& Env, std::string& OutMessage) const;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;

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
	// 기본(이름/부모) + 렌더 상태. 반환: 바뀌었으면 true (bOutTextures = 부모가 바뀜 → 텍스처 다시)
	bool DrawCommonSettings(FAssetEditorEnvironment& Env, bool& bOutTextures);
	void DrawFixedProperties(FAssetEditorEnvironment& Env);
	void DrawGraphProperties(FAssetEditorEnvironment& Env);
	// 인스턴스 파라미터 덮어쓰기 (체크 = 덮어쓰기). 반환: 바뀌었으면 true
	bool DrawParameterOverrides(bool& bOutTextures);
	// 일반 그래프 머티리얼 파라미터 목록 (추가/삭제/이름 변경/값)
	void DrawParameterList(FAssetEditorEnvironment& Env);
	void DrawHlslTab();
	// 해석(인스턴스는 부모 그래프) + 컴파일 + 분석
	void RefreshGraphStatus(FAssetEditorEnvironment& Env);
	// 일반 그래프 머티리얼: 편집 중 그래프만 다시 컴파일/분석 (해석 없음)
	void CompileOwnGraph();
	// 그래프 패널 편집 알림: Undo 한 단계 + (의미 변경이면) 컴파일, 해시가 같으면 즉시 반영 / 다르면 디바운스
	void OnGraphEdited(std::string_view Label, bool bSemantic);
	void LayoutMissingPositions(FMaterialGraph& Graph) const;
	// 자동 검증 --verify-matgraph-edit (Update에서 단계별)
	void VerifyEditStep(FAssetEditorEnvironment& Env);

	FMaterialAsset           Asset;
	FMaterialAsset           Inherited; // 인스턴스: 부모 체인만 해석한 값 (덮어쓰지 않은 항목에 보여 준다)
	std::string              ParentError;
	bool                     bGraphMaterial = false; // 해석 결과가 그래프 머티리얼 (인스턴스는 부모 그래프)
	FMaterialAsset           ResolvedGraph;          // 해석 결과 (인스턴스: 부모 그래프 + 전체 파라미터 — 읽기 전용 보기)
	FMaterialGraphCompileResult GraphCompile;
	FMaterialGraphAnalysis   GraphAnalysis;
	std::unique_ptr<FMaterialGraphPanel> GraphPanel;
	FAssetEditorEnvironment* DrawEnv = nullptr; // 그리기 동안만 (그래프 편집 알림이 쓴다)
	float                    ApplyDelay = -1.0f; // > 0이면 남은 시간 뒤 공유 머티리얼에 반영
	int32                    UpdateFrames = 0;
	bool                     bRoundTripVerified = false;
	int32                    VerifyEditStage    = 0; // --verify-matgraph-edit: 0 대기, 1 편집함, -1 끝
	uint64                   VerifyEditHash     = 0;
	std::string              ParameterNameEdit;
	int32                    ParameterNameEditIndex = -1;
	FMaterialHandle          Material;
	FEntity                  PreviewEntity;
	std::string              PreviewShape = "sphere";
	std::vector<std::string> TextureFiles;  // .emat 폴더 기준 상대 경로 ('/' 구분)
	std::vector<std::string> MaterialFiles; // 부모 후보 .emat (같은 기준, 자기 자신 제외)
};

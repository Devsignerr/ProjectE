#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Scene/AnimGraph.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ax::NodeEditor
{
	struct EditorContext;
}

// .eanimgraph 애니메이션 그래프 편집기 (UE 애니메이션 블루프린트 상태 머신식, imgui-node-editor).
//   - 위: 상태 노드 그래프. 노드 = 상태(클립 / 1D·2D 블렌드 스페이스) + "어느 상태든" 노드, 화살표 = 전이.
//     2D 블렌드는 속성 패널의 점 배치 그림에서 샘플(마름모)을 끌어 놓고, 빈 곳을 끌어 미리보기 값을 바꾼다 (초록 원 = 가중치)
//     출력 핀(오른쪽)을 다른 상태의 입력 핀(왼쪽)으로 끌면 전이 추가. 우클릭 메뉴(상태 추가/시작 상태/삭제), Delete = 선택 삭제
//   - 아래: 미리보기 모델(에셋의 PreviewModel, 비면 첫 상태 클립을 가진 Content 모델 자동 선택)을 편집 중인 그래프로 재생
//     (저장 전 편집도 즉시 반영). 파라미터 미리보기 값 슬라이더로 전이/블렌드를 시험한다
//   - 레이어: 그래프 위 콤보/오른쪽 '레이어'에서 고른 레이어의 상태 머신을 그래프에 보인다. 레이어 추가/순서/삭제, 가중치(+파라미터),
//     본 마스크(뼈 + 가중치 + 경사 깊이) 편집. 자동 검증 인자 --animgraph-layer <이름>
//   - 오른쪽: 파라미터 목록, 선택한 상태/전이 속성 (전이 조건·크로스페이드·종료 시점·우선순위 = 목록 순서), 블렌드 스페이스 축 편집
//   - 플레이 중: 이 에셋을 쓰는 엔티티(선택 엔티티 우선)의 현재 상태·섞이는 상태·방금 일어난 전이를 강조한다
//   저장하면 FAnimGraphLibrary::Invalidate → 이 그래프를 쓰는 컴포넌트가 새 그래프로 다시 묶인다 (핫 리로드, 파라미터 유지)
class FAnimGraphEditor final : public FAssetEditor
{
public:
	explicit FAnimGraphEditor(std::filesystem::path InPath);
	~FAnimGraphEditor() override;

	const char* GetTypeName() const override { return "애니메이션 그래프"; }
	float       GetPropertiesWidthWeight() const override { return 1.25f; }
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        FramePreview(FAssetEditorEnvironment& Env) override;

private:
	enum class ESelectionKind : uint8
	{
		None,
		State,
		Transition,
		AnyState,
	};
	struct FSelection
	{
		ESelectionKind Kind  = ESelectionKind::None;
		int32          Index = -1;
	};
	struct FGraphDeleter
	{
		void operator()(ax::NodeEditor::EditorContext* Context) const;
	};
	// 디버그 표시 대상 (플레이 중 엔티티 또는 미리보기). 상태/전이는 편집 중 에셋 번호로 바꾼 값
	struct FDebugView
	{
		const FAnimGraphRuntime* Runtime = nullptr;
		std::string              Label;
		bool                     bPlaying = false;
		int32                    CurrentState = -1;
		std::vector<std::pair<int32, float>> Layers; // (상태, 가중치)
		int32                    LastTransition = -1;
		uint32                   TransitionCount = 0;
	};

	// 미리보기 모델
	void EnsurePreviewModel(FAssetEditorEnvironment& Env);
	std::string FindAutoPreviewModel(FAssetEditorEnvironment& Env) const;
	void        PublishIfChanged();
	FAnimGraphRuntime* GetPreviewRuntime();

	// 그래프
	void DrawGraph(const FDebugView& Debug, float Height);
	void DrawStateNode(int32 Index, const FDebugView& Debug);
	void DrawAnyStateNode();
	void HandleGraphEdits();
	void DrawGraphMenus();
	void SyncPositionsFromGraph();
	void AutoLayoutMissing();
	FDebugView MakeDebugView(FAssetEditorEnvironment& Env);

	// 속성
	void DrawPreviewSection(FAssetEditorEnvironment& Env, const FDebugView& Debug);
	void DrawParameters();
	void DrawStateProperties(int32 Index);
	void DrawTransitionProperties(int32 Index);
	void DrawTransitionList(int32 FromState, bool bOnlyFrom);
	void DrawBlendSpaceAxis(FAnimGraphState& State);
	void DrawBlendSpace2D(FAnimGraphState& State); // 점 배치 그림 (샘플 끌기 + 미리보기 값 끌기 + 가중치 원)
	void MakeBlendSpace2D(FAnimGraphState& State); // Y축 파라미터 + 사각형 모서리 샘플 채우기
	// 첫 float 파라미터 (Exclude 제외), 없으면 Fallback 이름으로 추가
	std::string FindOrAddFloatParameter(const std::string& Fallback, const std::string& Exclude);
	bool ClipCombo(const char* Id, std::string& Clip);
	bool ParameterCombo(const char* Id, std::string& Parameter, bool bFloatOnly);
	void RenameParameter(const std::string& OldName, const std::string& NewName);
	float& PreviewValue(const std::string& Name);

	// 레이어: 그래프와 상태/전이 속성은 EditLayer의 상태 머신을 편집한다
	FAnimStateMachine&       Machine();
	const FAnimStateMachine& Machine() const;
	void                     SetEditLayer(int32 Layer); // -1 = 기본 레이어
	void                     DrawLayers(const FDebugView& Debug);
	void                     DrawMaskEditor(FAnimBoneMask& Mask, const char* Id);
	bool                     BoneCombo(const char* Id, std::string& Bone);

	int32       AddState(const std::string& BaseName, int32 BlendDimensions, const FVector2& Position); // 0 클립, 1/2 블렌드 스페이스
	std::string MakeUniqueStateName(const std::string& BaseName) const;
	void        MoveTransition(int32 Index, int32 Delta);

	FAnimGraphAsset                                               Asset;
	int32                                                         EditLayer = -1; // 그래프에 보이는 레이어 (-1 = 기본)
	std::unique_ptr<ax::NodeEditor::EditorContext, FGraphDeleter> Graph;
	FSelection                                                    Selection;
	bool                                                          bApplyPositions = true;
	int32                                                         NavigateFrames  = 2; // 0이 되는 프레임에 전체 보기
	uint32                                                        LastGraphNode   = 0;
	uintptr_t                                                     LastGraphLink   = 0;
	uint32                                                        ContextNodeId   = 0;
	uintptr_t                                                     ContextLinkId   = 0;
	FVector2                                                      ContextCanvasPosition;
	std::function<void()>                                         PendingChange; // 그래프 순회 뒤 적용할 구조 변경
	float                                                         GraphFraction = 0.58f; // 그래프 : 미리보기 높이 비율
	bool                                                          bSyncGraphSelection = false; // 속성 패널 선택 → 다음 그래프 프레임에 반영
	bool                                                          bScrollToSelection  = false; // 자동 검증: 속성 패널을 선택 항목까지 내린다
	int32                                                         AxisDragSample      = -1;    // 블렌드 축: 끄는 샘플 (-2 = 미리보기 값)
	float                                                         AxisLo = 0.0f, AxisHi = 1.0f; // 끄는 동안 고정한 축 범위
	float                                                         AxisLoY = 0.0f, AxisHiY = 1.0f; // 2D 블렌드의 Y축
	uint32                                                        SeenTransitionCount = 0;
	double                                                        TransitionFlashTime = -10.0; // 강조 시작 (ImGui 시각)
	int32                                                         FlashTransition     = -1;

	// 미리보기 (전용 씬)
	std::string                              LoadedModel; // 배치된 모델 (Content 기준)
	std::string                              AutoModel;   // PreviewModel이 비었을 때 고른 모델
	bool                                     bAutoModelSearched = false;
	FEntity                                  ModelRoot;
	std::vector<std::string>                 ClipNames;
	std::vector<std::string>                 BoneNames;  // 미리보기 모델 노드 이름 (본 마스크 콤보)
	std::vector<std::string>                 ModelFiles; // Content 안 모델 (콤보)
	std::shared_ptr<const FAnimGraphAsset>   Published;  // 미리보기가 재생하는 편집 상태 사본 (편집기 정보 제외 비교)
	std::string                              PublishedKey;
	std::vector<std::pair<std::string, float>> PreviewValues;
	bool                                     bPreviewPlaying = true;
	float                                    PreviewSpeed    = 1.0f;
};

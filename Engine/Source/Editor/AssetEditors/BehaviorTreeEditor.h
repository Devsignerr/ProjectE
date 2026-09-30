#pragma once

#include "AI/BehaviorTree/BehaviorTreeAsset.h"
#include "Editor/AssetEditors/AssetEditor.h"

#include <functional>
#include <memory>
#include <vector>

namespace ax::NodeEditor
{
	struct EditorContext;
}
class FBehaviorTreeInstance;
struct FBTNodeInfo;

// .ebt 비헤이비어 트리 편집기 (UE식 노드 그래프, imgui-node-editor).
//   - 그래프: 컴포지트/태스크가 노드, 데코레이터(위)/서비스(아래)는 노드 안의 줄. 링크는 위(부모) → 아래(자식)
//   - 자식 실행 순서 = 그래프에서의 X 좌표 순서 (UE와 같음). 노드를 옆으로 옮기면 순서가 바뀐다
//   - 트리는 항상 연결되어 있다: 노드는 부모 컴포지트의 우클릭 메뉴로만 만들고, 링크를 끌어 다른 컴포지트로 옮길 수 있다.
//     링크만 지우는 것은 막고, 노드 삭제(Delete)는 하위 트리째 지운다 (루트 제외)
//   - 오른쪽: 블랙보드 키 편집 + 선택한 노드/데코레이터/서비스의 파라미터 (레지스트리 FBTParamDesc로 위젯 결정)
//   - 플레이 중: 이 에셋을 쓰는 엔티티(선택 엔티티 우선)의 실행 중 노드를 강조하고 블랙보드 값을 보여 준다
//   노드 위치는 FBTNodeDesc::EditorPosition으로 에셋에 저장된다 (없으면 자동 배치)
class FBehaviorTreeEditor final : public FAssetEditor
{
public:
	explicit FBehaviorTreeEditor(std::filesystem::path InPath);
	~FBehaviorTreeEditor() override;

	const char* GetTypeName() const override { return "비헤이비어 트리"; }
	bool        UsesPreview() const override { return false; }
	float       GetPropertiesWidthWeight() const override { return 1.1f; }

	// 기본 에셋 (루트 Selector 하나) — 콘텐츠 브라우저 "새 비헤이비어 트리"
	static FBehaviorTreeAsset MakeDefaultAsset();

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawMainPanel(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        FramePreview(FAssetEditorEnvironment& Env) override;

private:
	enum class ESelectionKind : uint8
	{
		Node,
		Decorator,
		Service,
	};

	struct FSelection
	{
		uint32         NodeId = 0; // 0 = 선택 없음
		ESelectionKind Kind   = ESelectionKind::Node;
		int32          Index  = -1; // 데코레이터/서비스 번호
	};

	struct FGraphDeleter
	{
		void operator()(ax::NodeEditor::EditorContext* Context) const;
	};

	void DrawGraphNode(FBTNodeDesc& Node, int32 ChildOrder, const std::vector<uint32>& ActiveIds);
	void DrawGraphLinks(const FBTNodeDesc& Node, const std::vector<uint32>& ActiveIds);
	void HandleGraphEdits();
	void DrawGraphMenus();
	void SyncPositionsFromGraph();

	void DrawBlackboard(const FBehaviorTreeInstance* DebugTree);
	void DrawSelection();
	bool DrawParams(FBTNodeDesc& Desc, const FBTNodeInfo& Info);
	void DrawAttachmentList(FBTNodeDesc& Node, std::vector<FBTNodeDesc>& List, ESelectionKind Kind, const char* Label);
	bool DrawNodeTypeMenu(EBTNodeCategory Category, std::string& OutType);

	FBTNodeDesc*       FindSelectedDesc();
	const FBehaviorTreeInstance* FindDebugTree(FAssetEditorEnvironment& Env, std::string* OutEntityName) const;

	// 새 노드: 사용하지 않는 ID + (컴포지트/태스크면) 위치
	FBTNodeDesc MakeNode(const std::string& Type);
	void        AutoLayoutMissing();

	FBehaviorTreeAsset                                           Asset;
	std::unique_ptr<ax::NodeEditor::EditorContext, FGraphDeleter> Graph;
	FSelection                                                   Selection;
	bool                                                         bApplyPositions    = true; // 다음 그래프 프레임에 에셋 위치를 그래프에 넣는다
	int32                                                        NavigateFrames     = -1;   // 0이 되는 프레임에 전체 보기 (F, 버튼)
	int32                                                        CenterFrames       = 2;    // 0이 되는 프레임에 1:1 배율로 루트를 가운데 (열 때, 노드 크기가 잡힌 뒤)
	bool                                                         bSubItemClicked    = false; // 이번 프레임 노드 안 데코레이터/서비스 줄 클릭
	uint32                                                       LastGraphSelection = 0;     // 그래프 선택이 바뀔 때만 속성 선택을 따라간다
	uint32                                                       ContextNodeId      = 0;
	std::vector<uint32>                                          PreviousActiveIds; // 새로 활성화된 링크에 흐름 효과
	std::function<void()>                                        PendingChange;     // 그래프 순회가 끝난 뒤 적용할 구조 변경
};

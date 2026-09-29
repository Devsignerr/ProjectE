#pragma once

#include "Core/ECS/Entity.h"

#include <vector>

struct FEditorContext;

// 씬 계층 트리: 선택(외부 선택 시 자동 펼침/스크롤 + 강조), 드래그로 부모 변경, 우클릭 메뉴(자식 추가/삭제)
class FHierarchyPanel
{
public:
	void Draw(FEditorContext& Context);

	bool bOpen = true;

private:
	void DrawEntityNode(FEditorContext& Context, FEntity Entity);
	void DrawContextMenu(FEditorContext& Context, FEntity Entity);

	// 선택이 바뀌면(특히 뷰포트 클릭) 조상 노드를 펼치고 선택 노드로 스크롤한다
	FEntity LastSeenSelection;
	FEntity RevealTarget;
	bool    bScrollToReveal       = false;
	bool    bSelectedFromThisPanel = false;

	// 다중 선택: Shift+클릭 범위는 직전 프레임의 표시 순서 기준
	std::vector<FEntity> VisibleOrder;
	std::vector<FEntity> PreviousVisibleOrder;
	FEntity              SelectionAnchor;

	FEntity PendingDelete;
	bool    bPendingDuplicate = false;
	FEntity PendingReparentChild;
	FEntity PendingReparentParent;
	bool    bPendingReparent = false;
};

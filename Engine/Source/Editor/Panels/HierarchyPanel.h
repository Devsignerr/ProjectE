#pragma once

#include "Core/ECS/Entity.h"

struct FEditorContext;

// 씬 계층 트리: 선택, 드래그로 부모 변경, 우클릭 메뉴(자식 추가/삭제)
class FHierarchyPanel
{
public:
	void Draw(FEditorContext& Context);

	bool bOpen = true;

private:
	void DrawEntityNode(FEditorContext& Context, FEntity Entity);
	void DrawContextMenu(FEditorContext& Context, FEntity Entity);

	FEntity PendingDelete;
	FEntity PendingReparentChild;
	FEntity PendingReparentParent;
	bool    bPendingReparent = false;
};

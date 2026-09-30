#pragma once

#include "Core/ECS/Entity.h"

#include <filesystem>
#include <vector>

struct FEditorContext;

// 씬 계층 트리: 선택(외부 선택 시 자동 펼침/스크롤 + 강조), 드래그로 부모 변경, 우클릭 메뉴(자식 추가/삭제)
class FHierarchyPanel
{
public:
	void Draw(FEditorContext& Context);

	// 계층 창이 키보드 포커스를 가졌는지 (직전 프레임 기준, 씬 편집 단축키 대상 판정)
	bool IsFocused() const { return bFocused; }

	bool bOpen = true;

private:
	void DrawEntityNode(FEditorContext& Context, FEntity Entity);
	void DrawContextMenu(FEditorContext& Context, FEntity Entity);
	// 콘텐츠 브라우저에서 놓은 모델/파티클을 Parent 아래(없으면 루트)에 추가
	void AddAssets(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths, FEntity Parent);

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
	bool    bPendingCopy      = false;
	bool    bPendingPaste     = false;
	bool    bFocused          = false;
	FEntity PendingReparentChild;
	FEntity PendingReparentParent;
	bool    bPendingReparent = false;

	std::vector<std::filesystem::path> PendingAssetPaths;
	FEntity                            PendingAssetParent;
};

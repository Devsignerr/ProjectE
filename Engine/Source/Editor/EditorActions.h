#pragma once

struct FEditorContext;

// 여러 패널/단축키가 공유하는 선택 대상 편집 명령 (편집 알림으로 Undo 단계가 된다)
struct FEditorActions
{
	// 선택 엔티티(최상위, 자식 포함)를 같은 부모 아래 복제하고 복제본을 선택한다 (Ctrl+D)
	static void DuplicateSelection(FEditorContext& Context);

	// 선택 엔티티(최상위, 자식 포함)를 삭제하고 선택을 비운다 (Delete)
	static void DeleteSelection(FEditorContext& Context);

	// 파괴된 엔티티를 선택에서 뺀다
	static void PruneSelection(FEditorContext& Context);
};

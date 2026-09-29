#include "Editor/EditorActions.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneEditOps.h"
#include "Scene/Scene.h"

void FEditorActions::DuplicateSelection(FEditorContext& Context)
{
	PruneSelection(Context);
	if (Context.Selection.IsEmpty())
	{
		return;
	}
	// 주 선택의 복제본이 새 주 선택이 되도록 주 선택을 마지막에 둔다 (Duplicate는 입력 순서를 유지)
	const std::vector<FEntity> Clones = FSceneEditOps::Duplicate(*Context.Scene, Context.Selection.GetEntities());
	if (Clones.empty())
	{
		return;
	}
	Context.SelectMany(Clones, Clones.back());
	Context.MarkEdited("복제");
}

void FEditorActions::DeleteSelection(FEditorContext& Context)
{
	PruneSelection(Context);
	if (Context.Selection.IsEmpty())
	{
		return;
	}
	FSceneEditOps::Delete(*Context.Scene, Context.Selection.GetEntities());
	Context.ClearSelection();
	Context.MarkEdited("삭제");
}

void FEditorActions::PruneSelection(FEditorContext& Context)
{
	const FRegistry& Registry = Context.Scene->GetRegistry();
	Context.DeselectIf([&Registry](FEntity Entity) { return !Registry.IsValid(Entity); });
}

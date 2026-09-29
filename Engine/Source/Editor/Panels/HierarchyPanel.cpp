#include "Editor/Panels/HierarchyPanel.h"

#include "Editor/EditorActions.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneEditOps.h"
#include "Scene/Scene.h"

#include <imgui.h>

namespace
{
	constexpr const char* GEntityDragPayload = "E_ENTITY";
}

void FHierarchyPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	FScene&    Scene    = *Context.Scene;
	FRegistry& Registry = Scene.GetRegistry();

	// 선택 변경 감지: 이 패널 밖(뷰포트 등)에서 바뀌었으면 펼치고 스크롤
	if (Context.SelectedEntity != LastSeenSelection)
	{
		LastSeenSelection = Context.SelectedEntity;
		RevealTarget      = Context.SelectedEntity;
		bScrollToReveal   = !bSelectedFromThisPanel;
	}
	bSelectedFromThisPanel = false;

	VisibleOrder.clear();
	if (ImGui::Begin("계층", &bOpen))
	{
		// 루트 엔티티 목록 (순회 중 구조 변경을 피하기 위해 먼저 수집)
		std::vector<FEntity> Roots;
		Registry.View<FHierarchyComponent>().Each([&](FEntity Entity, FHierarchyComponent& Hierarchy) {
			if (!Hierarchy.Parent.IsValid())
			{
				Roots.push_back(Entity);
			}
		});

		for (FEntity Root : Roots)
		{
			DrawEntityNode(Context, Root);
		}

		// 빈 공간: 클릭 시 선택 해제, 드롭 시 루트로 이동, 우클릭 시 생성 메뉴
		ImGui::InvisibleButton("##HierarchyEmpty", ImVec2(FMath::Max(ImGui::GetContentRegionAvail().x, 1.0f),
		                                                  FMath::Max(ImGui::GetContentRegionAvail().y, 24.0f)));
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			Context.ClearSelection();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(GEntityDragPayload))
			{
				PendingReparentChild  = *static_cast<const FEntity*>(Payload->Data);
				PendingReparentParent = NullEntity;
				bPendingReparent      = true;
			}
			ImGui::EndDragDropTarget();
		}
		if (ImGui::BeginPopupContextItem("##HierarchyEmptyMenu"))
		{
			DrawContextMenu(Context, NullEntity);
			ImGui::EndPopup();
		}
	}
	ImGui::End();
	RevealTarget = NullEntity;
	PreviousVisibleOrder.swap(VisibleOrder);

	// 순회가 끝난 뒤 구조 변경 적용
	if (bPendingReparent)
	{
		// 선택된 엔티티를 끌면 선택된 최상위 엔티티 전부를 옮긴다
		const std::vector<FEntity> Children = Context.IsSelected(PendingReparentChild)
		                                          ? FSceneEditOps::GetTopLevel(Scene, Context.Selection.GetEntities())
		                                          : std::vector<FEntity>{ PendingReparentChild };
		for (FEntity Child : Children)
		{
			Scene.SetParent(Child, PendingReparentParent);
		}
		bPendingReparent = false;
		Context.MarkEdited("부모 변경");
	}
	if (PendingDelete.IsValid())
	{
		if (!Context.IsSelected(PendingDelete))
		{
			Context.Select(PendingDelete);
		}
		FEditorActions::DeleteSelection(Context);
		PendingDelete = NullEntity;
	}
	if (bPendingDuplicate)
	{
		FEditorActions::DuplicateSelection(Context);
		bPendingDuplicate = false;
	}
}

void FHierarchyPanel::DrawEntityNode(FEditorContext& Context, FEntity Entity)
{
	FScene&    Scene    = *Context.Scene;
	FRegistry& Registry = Scene.GetRegistry();

	const FNameComponent*      Name      = Registry.TryGet<FNameComponent>(Entity);
	const FHierarchyComponent* Hierarchy = Registry.TryGet<FHierarchyComponent>(Entity);
	const bool                 bLeaf     = Hierarchy == nullptr || Hierarchy->Children.empty();

	ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (bLeaf)
	{
		Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	}
	if (Context.IsSelected(Entity))
	{
		Flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::PushID(static_cast<int>(Entity.Index));
	VisibleOrder.push_back(Entity);

	// 선택 대상의 조상은 한 번 펼쳐 준다 (이후에는 사용자가 접을 수 있음)
	if (RevealTarget.IsValid() && !bLeaf && Scene.IsAncestorOf(Entity, RevealTarget))
	{
		ImGui::SetNextItemOpen(true);
	}

	// 선택 행은 뷰포트 아웃라인과 같은 계열 색으로 강조
	const bool bSelected = Context.IsSelected(Entity);
	if (bSelected)
	{
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.85f, 0.45f, 0.10f, 0.55f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.90f, 0.50f, 0.15f, 0.70f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.95f, 0.55f, 0.20f, 0.85f));
	}
	const bool bOpened = ImGui::TreeNodeEx("##Node", Flags, "%s", Name ? Name->Name.c_str() : "(이름 없음)");
	if (bSelected)
	{
		ImGui::PopStyleColor(3);
		if (bScrollToReveal && Entity == RevealTarget && Entity == Context.SelectedEntity)
		{
			ImGui::SetScrollHereY(0.5f);
			bScrollToReveal = false;
		}
	}

	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
	{
		const ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyShift && SelectionAnchor.IsValid())
		{
			// Shift+클릭: 앵커부터 범위 선택 (Ctrl을 함께 누르면 기존 선택에 추가)
			const std::vector<FEntity> Range = FEntitySelection::GetRange(PreviousVisibleOrder, SelectionAnchor, Entity);
			if (IO.KeyCtrl)
			{
				for (FEntity Ranged : Range)
				{
					Context.AddToSelection(Ranged);
				}
				Context.AddToSelection(Entity);
			}
			else
			{
				Context.SelectMany(Range, Entity);
			}
		}
		else if (IO.KeyCtrl)
		{
			Context.ToggleSelection(Entity);
			SelectionAnchor = Entity;
		}
		else
		{
			Context.Select(Entity);
			SelectionAnchor = Entity;
		}
		bSelectedFromThisPanel = true;
	}

	// 드래그 소스/타깃 (부모 변경)
	if (ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload(GEntityDragPayload, &Entity, sizeof(FEntity));
		ImGui::TextUnformatted(Name ? Name->Name.c_str() : "Entity");
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(GEntityDragPayload))
		{
			PendingReparentChild  = *static_cast<const FEntity*>(Payload->Data);
			PendingReparentParent = Entity;
			bPendingReparent      = true;
		}
		ImGui::EndDragDropTarget();
	}

	if (ImGui::BeginPopupContextItem("##EntityMenu"))
	{
		if (!Context.IsSelected(Entity))
		{
			Context.Select(Entity);
		}
		DrawContextMenu(Context, Entity);
		ImGui::EndPopup();
	}

	if (bOpened && !bLeaf)
	{
		const std::vector<FEntity> Children = Hierarchy->Children; // 순회 중 변경 대비 복사
		for (FEntity Child : Children)
		{
			DrawEntityNode(Context, Child);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void FHierarchyPanel::DrawContextMenu(FEditorContext& Context, FEntity Entity)
{
	FScene& Scene = *Context.Scene;

	if (ImGui::MenuItem("빈 엔티티 추가"))
	{
		const FEntity Created = Scene.CreateEntity("Entity");
		Scene.SetParent(Created, Entity);
		Context.Select(Created);
		Context.MarkEdited("엔티티 추가");
	}
	if (ImGui::MenuItem("큐브 추가"))
	{
		const FEntity Created = Scene.CreateEntity("Cube");
		Scene.SetParent(Created, Entity);
		FStaticMeshComponent& Mesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Created);
		Mesh.Mesh      = Context.DefaultCubeMesh;
		Mesh.MeshAsset = "primitive:cube";
		Context.Select(Created);
		Context.MarkEdited("큐브 추가");
	}
	if (Entity.IsValid())
	{
		ImGui::Separator();
		if (ImGui::MenuItem("복제", "Ctrl+D"))
		{
			bPendingDuplicate = true;
		}
		if (ImGui::MenuItem("삭제", "Del"))
		{
			PendingDelete = Entity;
		}
	}
}

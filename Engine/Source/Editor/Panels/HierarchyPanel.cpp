#include "Editor/Panels/HierarchyPanel.h"

#include "Editor/EditorContext.h"
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

	// 순회가 끝난 뒤 구조 변경 적용
	if (bPendingReparent)
	{
		Scene.SetParent(PendingReparentChild, PendingReparentParent);
		bPendingReparent = false;
	}
	if (PendingDelete.IsValid())
	{
		if (Context.SelectedEntity == PendingDelete || Scene.IsAncestorOf(PendingDelete, Context.SelectedEntity))
		{
			Context.ClearSelection();
		}
		Scene.DestroyEntity(PendingDelete);
		PendingDelete = NullEntity;
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
	if (Context.SelectedEntity == Entity)
	{
		Flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::PushID(static_cast<int>(Entity.Index));

	// 선택 대상의 조상은 한 번 펼쳐 준다 (이후에는 사용자가 접을 수 있음)
	if (RevealTarget.IsValid() && !bLeaf && Scene.IsAncestorOf(Entity, RevealTarget))
	{
		ImGui::SetNextItemOpen(true);
	}

	// 선택 행은 뷰포트 아웃라인과 같은 계열 색으로 강조
	const bool bSelected = Context.SelectedEntity == Entity;
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
		if (bScrollToReveal && Entity == RevealTarget)
		{
			ImGui::SetScrollHereY(0.5f);
			bScrollToReveal = false;
		}
	}

	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
	{
		Context.Select(Entity);
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
		Context.Select(Entity);
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
	}
	if (ImGui::MenuItem("큐브 추가"))
	{
		const FEntity Created = Scene.CreateEntity("Cube");
		Scene.SetParent(Created, Entity);
		FStaticMeshComponent& Mesh = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Created);
		Mesh.Mesh      = Context.DefaultCubeMesh;
		Mesh.MeshAsset = "primitive:cube";
		Context.Select(Created);
	}
	if (Entity.IsValid())
	{
		ImGui::Separator();
		if (ImGui::MenuItem("삭제", "Del"))
		{
			PendingDelete = Entity;
		}
	}
}

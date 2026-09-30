#include "Editor/Panels/HierarchyPanel.h"

#include "Editor/EditorActions.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/SceneEditOps.h"
#include "Audio/AudioComponents.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Renderer/ModelLoader.h"
#include "Scene/Particles.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>

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
	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_LIST, "계층", "Hierarchy").c_str(), &bOpen))
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
			// 콘텐츠 브라우저의 모델/파티클 → 루트에 추가
			if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
			{
				PendingAssetPaths  = *Paths;
				PendingAssetParent = NullEntity;
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
	if (!PendingAssetPaths.empty())
	{
		AddAssets(Context, PendingAssetPaths, PendingAssetParent);
		PendingAssetPaths.clear();
	}
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

namespace
{
	struct FEntityIcon
	{
		const char* Glyph = ICON_FA_CIRCLE_DOT;
		ImU32       Color = IM_COL32(140, 140, 140, 255);
	};

	// 가장 대표적인 컴포넌트로 아이콘/색을 고른다 (조명 > 카메라 > 파티클 > 모델 > 메시 > 오디오 > 스크립트)
	FEntityIcon GetEntityIcon(const FRegistry& Registry, FEntity Entity, bool bLeaf)
	{
		if (Registry.Has<FDirectionalLightComponent>(Entity)) return { ICON_FA_SUN, IM_COL32(250, 210, 90, 255) };
		if (Registry.Has<FCameraComponent>(Entity)) return { ICON_FA_VIDEO, IM_COL32(200, 200, 210, 255) };
		if (Registry.Has<FParticleSystemComponent>(Entity)) return { ICON_FA_FIRE, IM_COL32(255, 128, 64, 255) };
		if (Registry.Has<FModelComponent>(Entity)) return { ICON_FA_CUBES, IM_COL32(64, 170, 255, 255) };
		if (Registry.Has<FStaticMeshComponent>(Entity)) return { ICON_FA_CUBE, IM_COL32(110, 180, 240, 255) };
		if (Registry.Has<FAudioSourceComponent>(Entity)) return { ICON_FA_VOLUME_HIGH, IM_COL32(235, 110, 170, 255) };
		if (Registry.Has<FScriptComponent>(Entity)) return { ICON_FA_FILE_CODE, IM_COL32(80, 200, 200, 255) };
		return bLeaf ? FEntityIcon{} : FEntityIcon{ ICON_FA_FOLDER, IM_COL32(222, 178, 82, 255) };
	}
} // namespace

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

	// 선택 행은 테마 강조색 (언리얼 아웃라이너처럼 파랑)
	const bool bSelected = Context.IsSelected(Entity);
	if (bSelected)
	{
		ImGui::PushStyleColor(ImGuiCol_Header, FEditorTheme::Accent);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, FEditorTheme::AccentHover);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, FEditorTheme::AccentHover);
	}
	// 이름 앞 아이콘 자리를 비워 두고, 항목을 그린 뒤 종류별 색 아이콘을 덧그린다
	const bool bOpened = ImGui::TreeNodeEx("##Node", Flags, "      %s", Name ? Name->Name.c_str() : "(이름 없음)");
	{
		const FEntityIcon Icon   = GetEntityIcon(Registry, Entity, bLeaf);
		const ImVec2      ItemMin = ImGui::GetItemRectMin();
		const float       TextY   = ItemMin.y + ImGui::GetStyle().FramePadding.y;
		ImGui::GetWindowDrawList()->AddText(ImVec2(ItemMin.x + ImGui::GetTreeNodeToLabelSpacing(), TextY), Icon.Color, Icon.Glyph);
	}
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
		// 콘텐츠 브라우저의 모델/파티클 → 이 엔티티의 자식으로 추가
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
		{
			PendingAssetPaths  = *Paths;
			PendingAssetParent = Entity;
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

void FHierarchyPanel::AddAssets(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths, FEntity Parent)
{
	if (Context.bPlaying)
	{
		if (Context.Notify)
		{
			Context.Notify("플레이 중에는 에셋을 추가할 수 없습니다", true);
		}
		return;
	}
	FScene&              Scene = *Context.Scene;
	std::vector<FEntity> Added;
	for (const std::filesystem::path& Path : Paths)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		FEntity Entity;
		if (Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx")
		{
			Entity = FModelLoader::LoadIntoScene(Path, Scene, *Context.Resources, Parent);
		}
		else if (Extension == L".eparticle")
		{
			Entity = Scene.CreateEntity(FStringConv::ToUtf8(Path.stem().wstring()));
			Scene.GetRegistry().Emplace<FParticleSystemComponent>(Entity).Asset = FModelLoader::MakeAssetPath(Path);
			Scene.SetParent(Entity, Parent);
		}
		if (Scene.GetRegistry().IsValid(Entity))
		{
			Added.push_back(Entity);
		}
	}
	if (Added.empty())
	{
		if (Context.Notify)
		{
			Context.Notify("계층에는 모델과 파티클만 끌어 놓을 수 있습니다", true);
		}
		return;
	}
	Scene.UpdateTransforms();
	Context.SelectMany(Added, Added.back());
	Context.MarkEdited("에셋 추가");
}

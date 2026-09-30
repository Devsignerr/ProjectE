#include "Editor/AssetEditors/PrefabEditor.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/SceneEditOps.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Prefab.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::wstring LowerExtension(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension;
	}
} // namespace

void FPrefabEditor::DestroyContent()
{
	FScene& Scene = Preview.GetScene();
	if (Scene.GetRegistry().IsValid(Root))
	{
		Scene.DestroyEntity(Root); // 기본 조명은 별도 루트라 남는다
	}
	Root     = NullEntity;
	Selected = NullEntity;
}

void FPrefabEditor::ResolvePreviewAssets(FAssetEditorEnvironment& Env)
{
	if (Env.Resources != nullptr && Env.Editor != nullptr)
	{
		FSceneAssetResolver::Resolve(Preview.GetScene(), *Env.Resources, Env.Editor->ContentDirectory);
	}
}

bool FPrefabEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	// 되돌리기에서도 불린다: 이전 내용(루트 서브트리)만 지우고 파일 상태로 다시 펼친다
	FPrefabLibrary&   Library = FPrefabLibrary::Get();
	const std::string Asset   = Library.MakeAssetPath(Path);
	std::string       Error;
	if (Library.Load(Asset, &Error) == nullptr)
	{
		E_LOG(LogEditor, Error, "프리팹을 열지 못했습니다: {} — {}", Asset, Error);
		return false;
	}
	DestroyContent();
	Root     = Library.LoadAssetInto(Preview.GetScene(), Asset, NullEntity, NextId, &Error);
	Selected = Root;
	ResolvePreviewAssets(Env);
	return Preview.GetScene().GetRegistry().IsValid(Root);
}

bool FPrefabEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	FPrefabLibrary& Library = FPrefabLibrary::Get();
	std::string     Error;
	const auto      Save = [&] { return Library.SaveAsset(Preview.GetScene(), Root, Path, NextId, &Error); };
	const bool      bOk  = (Env.Editor != nullptr && Env.Editor->ChangePrefab) ? Env.Editor->ChangePrefab(Save) : Save();
	if (!bOk)
	{
		if (Env.Editor != nullptr && Env.Editor->Notify)
		{
			Env.Editor->Notify("프리팹을 저장하지 못했습니다: " + Error, true);
		}
		return false;
	}
	// 저장으로 새 엔티티에 ID가 붙었으므로 그 상태를 새 기준으로 (이전 단계로 되돌리면 ID가 다시 바뀌는 것을 막는다)
	History.Reset(CaptureState());
	if (Env.Editor != nullptr && Env.Editor->Notify)
	{
		Env.Editor->Notify("프리팹 저장 — 열린 씬의 인스턴스에 반영: " + GetDisplayName(), false);
	}
	return true;
}

std::string FPrefabEditor::CaptureState() const
{
	FScene& Scene = const_cast<FAssetPreview&>(Preview).GetScene();
	return FPrefabLibrary::Get().AssetStateToJson(Scene, Root, NextId);
}

void FPrefabEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	FScene&           Scene        = Preview.GetScene();
	const FEntityPath SelectedPath = FEntityPath::Build(Scene, Selected);
	DestroyContent();
	Root = FPrefabLibrary::Get().AssetStateFromJson(Scene, State, NullEntity, NextId);
	ResolvePreviewAssets(Env);
	Selected = SelectedPath.Resolve(Scene);
	if (!Scene.GetRegistry().IsValid(Selected))
	{
		Selected = Root;
	}
}

void FPrefabEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	FScene& Scene = Preview.GetScene();
	if (!Scene.GetRegistry().IsValid(Root))
	{
		ImGui::TextDisabled("프리팹 내용이 없습니다");
		return;
	}

	// 위: 프리팹 계층
	ImGui::SeparatorText(ICON_FA_LIST " 계층");
	const float TreeHeight = FMath::Max(ImGui::GetContentRegionAvail().y * 0.38f, 120.0f);
	if (ImGui::BeginChild("##PrefabTree", ImVec2(0.0f, TreeHeight), ImGuiChildFlags_Borders))
	{
		DrawNode(Env, Root);
	}
	ImGui::EndChild();
	ImGui::TextDisabled("콘텐츠의 .eprefab을 항목에 끌어 놓으면 중첩 프리팹으로 추가됩니다");
	ApplyPendingChanges(Env);

	// 아래: 선택 엔티티 인스펙터 (미리보기 씬 기준 임시 컨텍스트)
	ImGui::SeparatorText(ICON_FA_SLIDERS " 속성");
	if (!Scene.GetRegistry().IsValid(Selected))
	{
		Selected = Root;
	}
	FEditorContext Local;
	if (Env.Editor != nullptr)
	{
		Local.Scripts                = Env.Editor->Scripts;
		Local.ContentDirectory       = Env.Editor->ContentDirectory;
		Local.DefaultCubeMesh        = Env.Editor->DefaultCubeMesh;
		Local.Notify                 = Env.Editor->Notify;
		Local.OpenAssetEditorRequest = Env.Editor->OpenAssetEditorRequest;
	}
	Local.Rhi       = Env.Rhi;
	Local.Resources = Env.Resources;
	Local.Scene     = &Scene;
	Local.Select(Selected);
	if (ImGui::BeginChild("##PrefabInspector", ImVec2(0.0f, 0.0f)))
	{
		Inspector.DrawContents(Local);
	}
	ImGui::EndChild();
	std::string Label;
	if (Local.PendingEdit.TryTake(false, Label))
	{
		MarkEdited(Label); // 창 안 실행 취소 한 단계 (조작이 끝나면 커밋)
	}
}

void FPrefabEditor::DrawNode(FAssetEditorEnvironment& Env, FEntity Entity)
{
	FScene&                     Scene    = Preview.GetScene();
	FRegistry&                  Registry = Scene.GetRegistry();
	const FNameComponent*       Name     = Registry.TryGet<FNameComponent>(Entity);
	const std::vector<FEntity>& Children = Scene.GetChildren(Entity);
	const bool                  bNested  = Registry.Has<FPrefabInstanceComponent>(Entity);

	ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	if (Children.empty())
	{
		Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	}
	if (Entity == Selected)
	{
		Flags |= ImGuiTreeNodeFlags_Selected;
	}
	ImGui::PushID(static_cast<int>(Entity.Index));
	if (bNested)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::PrefabText);
	}
	const char* Icon    = bNested ? ICON_FA_BOXES_STACKED : (Entity == Root ? ICON_FA_CUBES : ICON_FA_CUBE);
	const bool  bOpened = ImGui::TreeNodeEx("##Node", Flags, "%s %s", Icon, Name ? Name->Name.c_str() : "(이름 없음)");
	if (bNested)
	{
		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("중첩 프리팹: %s", Registry.Get<FPrefabInstanceComponent>(Entity).Asset.c_str());
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
	{
		Selected = Entity;
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
		{
			PendingPrefabDrops = *Paths;
			PendingDropParent  = Entity;
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::BeginPopupContextItem("##PrefabNodeMenu"))
	{
		Selected = Entity;
		DrawNodeMenu(Env, Entity);
		ImGui::EndPopup();
	}
	if (bOpened && !Children.empty())
	{
		const std::vector<FEntity> Copy = Children;
		for (FEntity Child : Copy)
		{
			DrawNode(Env, Child);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void FPrefabEditor::DrawNodeMenu(FAssetEditorEnvironment& Env, FEntity Entity)
{
	(void)Env;
	if (ImGui::MenuItem("빈 엔티티 추가"))
	{
		PendingAddParent = Entity;
		PendingAddKind   = "empty";
	}
	if (ImGui::MenuItem("큐브 추가"))
	{
		PendingAddParent = Entity;
		PendingAddKind   = "cube";
	}
	if (Entity != Root)
	{
		ImGui::Separator();
		if (ImGui::MenuItem("복제"))
		{
			PendingDuplicate = Entity;
		}
		if (ImGui::MenuItem("삭제"))
		{
			PendingDelete = Entity;
		}
	}
}

void FPrefabEditor::ApplyPendingChanges(FAssetEditorEnvironment& Env)
{
	FScene&    Scene    = Preview.GetScene();
	FRegistry& Registry = Scene.GetRegistry();
	if (!PendingAddKind.empty() && Registry.IsValid(PendingAddParent))
	{
		const bool    bCube   = PendingAddKind == "cube";
		const FEntity Created = Scene.CreateEntity(bCube ? "Cube" : "Entity");
		Scene.SetParent(Created, PendingAddParent);
		if (bCube)
		{
			Registry.Emplace<FStaticMeshComponent>(Created).MeshAsset = "primitive:cube";
			ResolvePreviewAssets(Env);
		}
		Selected = Created;
		MarkEdited(bCube ? "큐브 추가" : "엔티티 추가");
	}
	PendingAddKind.clear();
	if (Registry.IsValid(PendingDuplicate) && PendingDuplicate != Root)
	{
		const std::vector<FEntity> Clones = FSceneEditOps::Duplicate(Scene, { PendingDuplicate });
		Selected                          = Clones.empty() ? Selected : Clones.front();
		MarkEdited("복제");
	}
	PendingDuplicate = NullEntity;
	if (Registry.IsValid(PendingDelete) && PendingDelete != Root)
	{
		Scene.DestroyEntity(PendingDelete);
		Selected = Root;
		MarkEdited("삭제");
	}
	PendingDelete = NullEntity;
	if (!PendingPrefabDrops.empty())
	{
		for (const std::filesystem::path& File : PendingPrefabDrops)
		{
			AddNestedPrefab(Env, File, PendingDropParent);
		}
		PendingPrefabDrops.clear();
	}
}

void FPrefabEditor::AddNestedPrefab(FAssetEditorEnvironment& Env, const std::filesystem::path& File, FEntity Parent)
{
	const auto Notify = [&Env](const std::string& Message) {
		if (Env.Editor != nullptr && Env.Editor->Notify)
		{
			Env.Editor->Notify(Message, true);
		}
	};
	if (LowerExtension(File) != FPrefabLibrary::Extension)
	{
		Notify("프리팹 편집 창에는 .eprefab만 끌어 놓을 수 있습니다");
		return;
	}
	FPrefabLibrary&   Library = FPrefabLibrary::Get();
	const std::string Asset   = Library.MakeAssetPath(File);
	const std::string Self    = Library.MakeAssetPath(Path);
	// 순환 거부: 넣으려는 프리팹이 (중첩을 따라가며) 이 프리팹을 포함하면 안 된다
	if (Library.DependsOn(Asset, Self))
	{
		Notify("프리팹이 자기 자신을 포함하게 되어 넣을 수 없습니다: " + Asset);
		return;
	}
	std::string   Error;
	const FEntity Nested = Library.Instantiate(Preview.GetScene(), Asset, Preview.GetScene().GetRegistry().IsValid(Parent) ? Parent : Root, &Error);
	if (!Nested.IsValid())
	{
		Notify("프리팹을 넣지 못했습니다: " + Error);
		return;
	}
	ResolvePreviewAssets(Env);
	Selected = Nested;
	MarkEdited("중첩 프리팹 추가");
}

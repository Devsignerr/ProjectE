#include "Editor/EditorActions.h"

#include "Core/StringConv.h"
#include "Editor/ContentBrowser/AssetFileOps.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneEditOps.h"
#include "Editor/SelectionOutline.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <format>
#include <unordered_set>

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

void FEditorActions::CopySelection(FEditorContext& Context)
{
	PruneSelection(Context);
	if (Context.Selection.IsEmpty())
	{
		return;
	}
	Context.EntityClipboard = FSceneEditOps::Copy(*Context.Scene, Context.Selection.GetEntities());
	if (Context.Notify)
	{
		Context.Notify(std::format("엔티티 {}개 복사", FSceneEditOps::GetTopLevel(*Context.Scene, Context.Selection.GetEntities()).size()), false);
	}
}

void FEditorActions::PasteClipboard(FEditorContext& Context)
{
	const std::vector<FEntity> Pasted = FSceneEditOps::Paste(*Context.Scene, Context.EntityClipboard);
	if (Pasted.empty())
	{
		return;
	}
	// 모델 하위 노드/메시·머티리얼 핸들은 저장되지 않으므로 다시 해석한다
	if (Context.Resources != nullptr)
	{
		FSceneAssetResolver::Resolve(*Context.Scene, *Context.Resources, Context.ContentDirectory);
		Context.Scene->UpdateTransforms();
	}
	Context.SelectMany(Pasted, Pasted.back());
	Context.MarkEdited("붙여넣기");
}

void FEditorActions::SnapSelectionToFloor(FEditorContext& Context)
{
	PruneSelection(Context);
	if (Context.Selection.IsEmpty() || Context.Resources == nullptr)
	{
		return;
	}
	FScene&                    Scene    = *Context.Scene;
	FRegistry&                 Registry = Scene.GetRegistry();
	const std::vector<FEntity> Targets  = FSceneEditOps::GetTopLevel(Scene, Context.Selection.GetEntities());

	const auto GetMeshBounds = [&](FEntity Entity, const FStaticMeshComponent& MeshComponent) {
		const FStaticMesh* Mesh = Context.Resources->GetMesh(MeshComponent.Mesh);
		return Mesh ? Mesh->GetLocalBounds().TransformBy(Scene.GetTransform(Entity).WorldMatrix) : FBox();
	};

	// 대상 자신(하위 메시 포함)은 바닥 후보에서 뺀다
	std::vector<std::vector<FEntity>> TargetMeshes(Targets.size());
	std::unordered_set<uint64>        Excluded;
	for (size_t Index = 0; Index < Targets.size(); ++Index)
	{
		FSelectionOutline::CollectOutlinedEntities(Scene, Targets[Index], TargetMeshes[Index]);
		for (FEntity Mesh : TargetMeshes[Index])
		{
			Excluded.insert(Mesh.ToId());
		}
	}
	std::vector<FBox> Surfaces;
	Registry.View<FTransformComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FTransformComponent&, FStaticMeshComponent& MeshComponent) {
		if (MeshComponent.bVisible && !Excluded.contains(Entity.ToId()))
		{
			Surfaces.push_back(GetMeshBounds(Entity, MeshComponent));
		}
	});

	bool bFoundFloor = false;
	bool bMoved      = false;
	for (size_t Index = 0; Index < Targets.size(); ++Index)
	{
		const FEntity Target = Targets[Index];
		FBox          Bounds;
		for (FEntity Mesh : TargetMeshes[Index])
		{
			Bounds.AddBox(GetMeshBounds(Mesh, Registry.Get<FStaticMeshComponent>(Mesh)));
		}
		FTransformComponent& Transform = Scene.GetTransform(Target);
		if (!Bounds.IsValid())
		{
			Bounds.AddPoint(Transform.GetWorldPosition()); // 메시가 없으면 피벗 기준
		}
		float Floor = 0.0f;
		if (!FSceneEditOps::FindFloorHeight(Bounds, Surfaces, Floor))
		{
			continue;
		}
		bFoundFloor = true;
		if (FMath::Abs(Floor - Bounds.Min.Z) < 1.0e-4f)
		{
			continue; // 이미 붙어 있음
		}
		// 월드에서 Z로 옮긴 뒤 로컬로: Local = World * Inverse(ParentWorld)
		const FMatrix4x4 World = Transform.WorldMatrix * FMatrix4x4::MakeTranslation(FVector3(0.0f, 0.0f, Floor - Bounds.Min.Z));
		const FMatrix4x4 Local = World * Scene.GetParentWorldMatrix(Target).GetInverse();
		Local.Decompose(Transform.Position, Transform.Rotation, Transform.Scale);
		bMoved = true;
	}
	if (!bFoundFloor && Context.Notify)
	{
		Context.Notify("아래에 붙일 바닥이 없습니다", false);
	}
	if (!bMoved)
	{
		return;
	}
	Scene.UpdateTransforms();
	Context.MarkEdited("바닥에 붙이기");
}

void FEditorActions::PruneSelection(FEditorContext& Context)
{
	const FRegistry& Registry = Context.Scene->GetRegistry();
	Context.DeselectIf([&Registry](FEntity Entity) { return !Registry.IsValid(Entity); });
}

std::vector<std::filesystem::path> FEditorActions::CreatePrefabs(FEditorContext& Context, FEntity Entity, const std::filesystem::path& Directory)
{
	const auto Notify = [&Context](const std::string& Message, bool bError) {
		if (Context.Notify)
		{
			Context.Notify(Message, bError);
		}
	};
	std::vector<std::filesystem::path> Created;
	if (Context.bPlaying)
	{
		Notify("플레이 중에는 프리팹을 만들 수 없습니다", true);
		return Created;
	}
	FScene&                    Scene   = *Context.Scene;
	const std::vector<FEntity> Sources = Context.IsSelected(Entity) ? FSceneEditOps::GetTopLevel(Scene, Context.Selection.GetEntities()) : std::vector<FEntity>{ Entity };
	std::string                Errors;
	for (FEntity Source : Sources)
	{
		const FNameComponent* Name = Scene.GetRegistry().TryGet<FNameComponent>(Source);
		std::wstring          Stem = Name ? FStringConv::ToWide(Name->Name) : std::wstring();
		if (!FAssetFileOps::IsValidName(Stem))
		{
			Stem = L"Prefab";
		}
		const std::filesystem::path File = FAssetFileOps::MakeUniquePath(Directory, Stem, FPrefabLibrary::Extension);
		std::string                 Error;
		if (FPrefabLibrary::Get().CreatePrefab(Scene, Source, File, &Error))
		{
			Created.push_back(File);
		}
		else
		{
			Errors += (Errors.empty() ? "" : ", ") + Error;
		}
	}
	if (!Created.empty())
	{
		Context.MarkEdited("프리팹 만들기");
		Notify(std::format("프리팹 {}개 만듦: {}", Created.size(), FStringConv::ToUtf8(Created.front().filename().wstring())), false);
	}
	if (!Errors.empty())
	{
		Notify("프리팹을 만들지 못했습니다 — " + Errors, true);
	}
	return Created;
}

FEntity FEditorActions::InstantiatePrefab(FEditorContext& Context, const std::filesystem::path& Path, FEntity Parent)
{
	std::string     Error;
	FPrefabLibrary& Library = FPrefabLibrary::Get();
	const FEntity   Root    = Library.Instantiate(*Context.Scene, Library.MakeAssetPath(Path), Parent, &Error);
	if (!Root.IsValid())
	{
		if (Context.Notify)
		{
			Context.Notify("프리팹을 놓지 못했습니다: " + Error, true);
		}
		return NullEntity;
	}
	if (Context.Resources != nullptr)
	{
		FSceneAssetResolver::Resolve(*Context.Scene, *Context.Resources, Context.ContentDirectory);
	}
	return Root;
}

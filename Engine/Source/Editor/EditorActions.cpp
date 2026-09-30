#include "Editor/EditorActions.h"

#include "Core/StringConv.h"
#include "Editor/ContentBrowser/AssetFileOps.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneEditOps.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <format>

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
	std::string   Error;
	FPrefabLibrary& Library = FPrefabLibrary::Get();
	const FEntity Root    = Library.Instantiate(*Context.Scene, Library.MakeAssetPath(Path), Parent, &Error);
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

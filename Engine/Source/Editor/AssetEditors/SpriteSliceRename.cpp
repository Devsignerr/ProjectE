#include "Editor/AssetEditors/SpriteSliceRename.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <algorithm>
#include <cwctype>
#include <format>
#include <fstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::wstring LowerExtension(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension;
	}

	bool WriteText(const std::filesystem::path& Path, const std::string& Text)
	{
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
		return static_cast<bool>(File);
	}

	struct FPendingWrite
	{
		std::filesystem::path Path;
		std::string           Text;
		int32                 Count = 0;
	};
} // namespace

SpriteSliceRename::FResult SpriteSliceRename::Propagate(FEditorContext& Context, const std::string& AtlasPath,
                                                         std::span<const Sprite2DEditing::FSliceRename> Renames)
{
	FResult Result;
	if (Renames.empty() || Context.ContentDirectory.empty())
	{
		return Result;
	}
	const std::filesystem::path OpenScene = Context.GetScenePath ? Context.GetScenePath() : std::filesystem::path();

	// 1) Content 훑기 → 바꿀 파일 텍스트 (쓰기는 종류별로 아래에서)
	std::vector<FPendingWrite> Flipbooks;
	std::vector<FPendingWrite> Prefabs;
	std::vector<FPendingWrite> Scenes;
	std::error_code            ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(Context.ContentDirectory, ErrorCode);
	     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
	{
		if (!It->is_regular_file(ErrorCode))
		{
			continue;
		}
		const std::filesystem::path& Path      = It->path();
		const std::wstring           Extension = LowerExtension(Path);
		const bool bFlipbook = Extension == L".eflipbook";
		const bool bPrefab   = Extension == FPrefabLibrary::Extension;
		const bool bScene    = Extension == L".escene";
		if (!bFlipbook && !bPrefab && !bScene)
		{
			continue;
		}
		if (bScene && !OpenScene.empty() && std::filesystem::equivalent(Path, OpenScene, ErrorCode))
		{
			continue; // 열린 씬은 메모리에서 (아래 3)
		}
		std::string Text;
		if (!FFileSystem::ReadTextFile(Path, Text))
		{
			continue;
		}
		std::string NewText;
		const int32 Count = bFlipbook ? Sprite2DEditing::RenameSliceRefsInFlipbook(Text, FPrefabLibrary::Get().MakeAssetPath(Path), AtlasPath, Renames, NewText)
		                              : Sprite2DEditing::RenameSliceRefsInEntityJson(Text, AtlasPath, Renames, NewText);
		if (Count > 0)
		{
			(bFlipbook ? Flipbooks : bPrefab ? Prefabs : Scenes).push_back({ Path, std::move(NewText), Count });
		}
	}

	const auto Write = [&Result](const FPendingWrite& Pending) {
		if (!WriteText(Pending.Path, Pending.Text))
		{
			E_LOG(LogEditor, Error, "[슬라이스 이름 변경] 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Pending.Path.wstring()));
			return false;
		}
		Result.ChangedFiles.push_back(Pending.Path);
		Result.ChangedReferences += Pending.Count;
		return true;
	};
	for (const FPendingWrite& Pending : Flipbooks)
	{
		if (Write(Pending))
		{
			FSprite2DLibrary::Get().Invalidate(FPrefabLibrary::Get().MakeAssetPath(Pending.Path));
		}
	}
	for (const FPendingWrite& Pending : Scenes)
	{
		Write(Pending);
	}
	// 2) 프리팹: 원본을 바꾸는 작업은 ChangePrefab 안에서 (열린 씬 인스턴스가 새 원본에 맞춰진다)
	if (!Prefabs.empty())
	{
		const auto WritePrefabs = [&]() {
			bool bOk = true;
			for (const FPendingWrite& Pending : Prefabs)
			{
				bOk = Write(Pending) && bOk;
			}
			return bOk;
		};
		if (Context.ChangePrefab)
		{
			Context.ChangePrefab(WritePrefabs);
		}
		else
		{
			WritePrefabs();
			FPrefabLibrary::Get().Invalidate();
		}
	}

	// 3) 열린 씬 (메모리 — Undo 한 단계). 플레이 중에는 Context.Scene이 플레이 복제본이라 고치지 않는다
	if (Context.bPlaying)
	{
		Result.bSkippedOpenScene = true;
	}
	else if (Context.Scene != nullptr)
	{
		Context.Scene->GetRegistry().View<FSpriteComponent>().Each([&](FEntity, FSpriteComponent& Sprite) {
			if (Sprite2DEditing::IsSameAssetPath(Sprite.Sprite, AtlasPath) && Sprite2DEditing::ApplySliceRename(Sprite.Slice, Renames))
			{
				++Result.OpenSceneComponents;
			}
		});
		if (Result.OpenSceneComponents > 0)
		{
			Context.MarkEdited("슬라이스 이름 변경 반영");
		}
	}

	E_LOG(LogEditor, Display, "[슬라이스 이름 변경] {} — {}", AtlasPath, Describe(Result));
	for (const std::filesystem::path& Path : Result.ChangedFiles)
	{
		E_LOG(LogEditor, Display, "[슬라이스 이름 변경]   {}", FStringConv::ToUtf8(Path.lexically_relative(Context.ContentDirectory).generic_wstring()));
	}
	return Result;
}

std::string SpriteSliceRename::Describe(const FResult& Result)
{
	std::string Text = std::format("파일 {}개(참조 {}개), 열린 씬 {}개", Result.ChangedFiles.size(), Result.ChangedReferences, Result.OpenSceneComponents);
	if (Result.bSkippedOpenScene)
	{
		Text += " (플레이 중이라 열린 씬은 건너뜀)";
	}
	return Text;
}

#include "Scene/Sprite/Sprite2DLibrary.h"

#include "Core/FileSystem.h"
#include "Core/StringConv.h"
#include "Scene/Prefab.h"

#include <algorithm>
#include <cwctype>
#include <fstream>

E_DEFINE_LOG_CATEGORY(LogSprite2D, Log)

namespace
{
	std::wstring LowerWide(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}
} // namespace

FSprite2DLibrary& FSprite2DLibrary::Get()
{
	static FSprite2DLibrary Instance; // 엔진 DLL 안 하나 (헤더 인라인 static 금지 — 바이너리마다 따로 생긴다)
	return Instance;
}

std::filesystem::path FSprite2DLibrary::ResolvePath(const std::string& AssetPath) const
{
	return FPrefabLibrary::Get().ResolveAssetPath(AssetPath);
}

std::wstring FSprite2DLibrary::MakeKey(const std::string& AssetPath)
{
	return LowerWide(FPrefabLibrary::Get().ResolveAssetPath(AssetPath).lexically_normal().generic_wstring());
}

bool FSprite2DLibrary::IsSprite2DExtension(const std::wstring& LowerExtension)
{
	return LowerExtension == FSpriteAsset::Extension || LowerExtension == FFlipbookAsset::Extension || LowerExtension == FTilesetAsset::Extension;
}

std::string FSprite2DLibrary::ResolveReference(const std::string& OwnerAssetPath, const std::string& Reference)
{
	if (Reference.empty())
	{
		return {};
	}
	const std::filesystem::path Ref = FStringConv::ToWide(Reference);
	const std::filesystem::path Absolute =
		Ref.is_absolute() ? Ref.lexically_normal() : (FPrefabLibrary::Get().ResolveAssetPath(OwnerAssetPath).parent_path() / Ref).lexically_normal();
	return FPrefabLibrary::Get().MakeAssetPath(Absolute);
}

void FSprite2DLibrary::Invalidate()
{
	Sprites.clear();
	Flipbooks.clear();
	Tilesets.clear();
	++Generation;
}

void FSprite2DLibrary::Invalidate(const std::string& AssetPath)
{
	const std::wstring Key = MakeKey(AssetPath);
	Sprites.erase(Key);
	Flipbooks.erase(Key);
	Tilesets.erase(Key);
	++Generation;
}

void FSprite2DLibrary::SetSpritePreview(const std::string& AssetPath, std::shared_ptr<const FSpriteAsset> Asset)
{
	Sprites[MakeKey(AssetPath)] = std::move(Asset);
	++Generation;
}

void FSprite2DLibrary::SetFlipbookPreview(const std::string& AssetPath, std::shared_ptr<const FFlipbookAsset> Asset)
{
	Flipbooks[MakeKey(AssetPath)] = std::move(Asset);
	++Generation;
}

template <typename TAsset>
std::shared_ptr<const TAsset> FSprite2DLibrary::LoadCached(std::unordered_map<std::wstring, std::shared_ptr<const TAsset>>& Cache, const std::string& AssetPath,
                                                           const char* Kind)
{
	if (AssetPath.empty())
	{
		return nullptr;
	}
	const std::wstring Key = MakeKey(AssetPath);
	if (const auto Found = Cache.find(Key); Found != Cache.end())
	{
		return Found->second;
	}
	std::shared_ptr<const TAsset> Result;
	const std::string             Display = FPrefabLibrary::Get().MakeAssetPath(ResolvePath(AssetPath));
	std::string                   Text;
	if (!FFileSystem::ReadTextFile(ResolvePath(AssetPath), Text))
	{
		E_LOG(LogSprite2D, Warning, "[2D] {}을(를) 읽을 수 없습니다: {}", Kind, Display);
	}
	else
	{
		auto                     Asset = std::make_shared<TAsset>();
		std::vector<std::string> Warnings;
		std::string              Error;
		if (TAsset::FromJsonString(Text, *Asset, &Warnings, &Error))
		{
			for (const std::string& Warning : Warnings)
			{
				E_LOG(LogSprite2D, Warning, "[2D] {}: {}", Display, Warning);
			}
			Result = std::move(Asset);
		}
		else
		{
			E_LOG(LogSprite2D, Warning, "[2D] {} 형식 오류 ({}): {}", Kind, Display, Error);
		}
	}
	Cache[Key] = Result;
	return Result;
}

std::shared_ptr<const FSpriteAsset> FSprite2DLibrary::LoadSprite(const std::string& AssetPath)
{
	return LoadCached(Sprites, AssetPath, "스프라이트");
}

std::shared_ptr<const FFlipbookAsset> FSprite2DLibrary::LoadFlipbook(const std::string& AssetPath)
{
	return LoadCached(Flipbooks, AssetPath, "플립북");
}

std::shared_ptr<const FTilesetAsset> FSprite2DLibrary::LoadTileset(const std::string& AssetPath)
{
	return LoadCached(Tilesets, AssetPath, "타일셋");
}

bool FSprite2DLibrary::SaveText(const std::string& AssetPath, const std::string& Text, std::string* OutError)
{
	const std::filesystem::path Path = ResolvePath(AssetPath);
	std::error_code             ErrorCode;
	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	}
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (File)
	{
		File << Text;
	}
	if (!File)
	{
		if (OutError != nullptr)
		{
			*OutError = "파일을 쓸 수 없습니다: " + FStringConv::ToUtf8(Path.generic_wstring());
		}
		return false;
	}
	File.close();
	Invalidate(AssetPath);
	return true;
}

bool FSprite2DLibrary::SaveSprite(const std::string& AssetPath, const FSpriteAsset& Asset, std::string* OutError)
{
	return SaveText(AssetPath, Asset.ToJsonString(), OutError);
}

bool FSprite2DLibrary::SaveFlipbook(const std::string& AssetPath, const FFlipbookAsset& Asset, std::string* OutError)
{
	return SaveText(AssetPath, Asset.ToJsonString(), OutError);
}

bool FSprite2DLibrary::SaveTileset(const std::string& AssetPath, const FTilesetAsset& Asset, std::string* OutError)
{
	FTilesetAsset Normalized = Asset;
	Normalized.Normalize();
	return SaveText(AssetPath, Normalized.ToJsonString(), OutError);
}

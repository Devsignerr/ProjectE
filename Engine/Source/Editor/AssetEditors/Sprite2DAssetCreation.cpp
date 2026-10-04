#include "Editor/AssetEditors/Sprite2DAssetCreation.h"

#include "Core/FileSystem.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/SpriteAtlasEditor.h"
#include "Editor/ContentBrowser/AssetFileOps.h"
#include "Editor/EditorTheme.h"
#include "Renderer/Image.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>

namespace Sprite2DAssetCreation
{
	namespace
	{
		bool ReadImageSize(const std::filesystem::path& Image, int32& OutWidth, int32& OutHeight)
		{
			std::vector<uint8> Bytes;
			FImage             Decoded;
			if (!FFileSystem::ReadFile(Image, Bytes) || !FImageLoader::LoadFromMemory(Bytes.data(), Bytes.size(), Decoded, "sprite2d"))
			{
				return false;
			}
			OutWidth  = static_cast<int32>(Decoded.Width);
			OutHeight = static_cast<int32>(Decoded.Height);
			return true;
		}

		std::string ToUtf8(const std::filesystem::path& Path) { return FStringConv::ToUtf8(Path.wstring()); }
	} // namespace

	bool IsImageFile(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension == L".png" || Extension == L".jpg" || Extension == L".jpeg" || Extension == L".tga" || Extension == L".bmp";
	}

	std::filesystem::path CreateSpriteAtlas(const std::filesystem::path& Image, bool bGridDialog, std::string* OutError)
	{
		FSpriteAsset Asset;
		Asset.Texture = FStringConv::ToUtf8(Image.filename().wstring());
		if (!ReadImageSize(Image, Asset.TextureWidth, Asset.TextureHeight))
		{
			if (OutError != nullptr)
			{
				*OutError = "이미지를 읽을 수 없습니다: " + ToUtf8(Image.filename());
			}
			return {};
		}
		FSpriteSlice Slice;
		Slice.Name = FStringConv::ToUtf8(Image.stem().wstring());
		Slice.W    = Asset.TextureWidth;
		Slice.H    = Asset.TextureHeight;
		Asset.Slices.push_back(std::move(Slice));
		const std::filesystem::path Path = FAssetFileOps::MakeUniquePath(Image.parent_path(), Image.stem().wstring(), FSpriteAsset::Extension);
		if (!FSprite2DLibrary::Get().SaveSprite(ToUtf8(Path), Asset, OutError))
		{
			return {};
		}
		if (bGridDialog)
		{
			FSpriteAtlasEditor::RequestGridDialog(Path);
		}
		return Path;
	}

	std::filesystem::path CreateTileset(const std::filesystem::path& Image, int32 TileWidth, int32 TileHeight, std::string* OutError)
	{
		FTilesetAsset Asset;
		Asset.Texture    = FStringConv::ToUtf8(Image.filename().wstring());
		Asset.TileWidth  = FMath::Max(TileWidth, 1);
		Asset.TileHeight = FMath::Max(TileHeight, 1);
		if (!ReadImageSize(Image, Asset.TextureWidth, Asset.TextureHeight))
		{
			if (OutError != nullptr)
			{
				*OutError = "이미지를 읽을 수 없습니다: " + ToUtf8(Image.filename());
			}
			return {};
		}
		const std::filesystem::path Path = FAssetFileOps::MakeUniquePath(Image.parent_path(), Image.stem().wstring(), FTilesetAsset::Extension);
		if (!FSprite2DLibrary::Get().SaveTileset(ToUtf8(Path), Asset, OutError))
		{
			return {};
		}
		return Path;
	}

	bool SaveDefaultFlipbook(const std::filesystem::path& Path)
	{
		return FSprite2DLibrary::Get().SaveFlipbook(ToUtf8(Path), FFlipbookAsset{});
	}

	std::filesystem::path DrawImageContextMenu(const std::filesystem::path& Image, std::string& OutError)
	{
		if (!IsImageFile(Image))
		{
			return {};
		}
		std::filesystem::path Created;
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FA_TABLE_CELLS " 스프라이트 아틀라스 만들기"))
		{
			Created = CreateSpriteAtlas(Image, false, &OutError);
		}
		ImGui::SetItemTooltip("같은 폴더에 .esprite (전체 이미지 슬라이스 하나)");
		if (ImGui::MenuItem(ICON_FA_SCISSORS " 스프라이트 아틀라스 만들기 (격자로 자르기)"))
		{
			Created = CreateSpriteAtlas(Image, true, &OutError);
		}
		if (ImGui::BeginMenu(ICON_FA_BORDER_ALL " 타일셋 만들기"))
		{
			static int32 TileSize[2] = { 16, 16 };
			ImGui::SetNextItemWidth(140.0f);
			if (ImGui::InputInt2("타일 W, H (px)", TileSize))
			{
				TileSize[0] = FMath::Clamp(TileSize[0], 1, 4096);
				TileSize[1] = FMath::Clamp(TileSize[1], 1, 4096);
			}
			if (ImGui::MenuItem("만들기"))
			{
				Created = CreateTileset(Image, TileSize[0], TileSize[1], &OutError);
			}
			ImGui::EndMenu();
		}
		return Created;
	}
} // namespace Sprite2DAssetCreation

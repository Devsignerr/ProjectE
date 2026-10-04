#include "Scene/Sprite/TilesetAsset.h"

#include "Scene/Sprite/Sprite2DJson.h"

#include <algorithm>
#include <cctype>

using namespace Sprite2DJson;

namespace
{
	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		return A.size() == B.size() &&
		       std::equal(A.begin(), A.end(), B.begin(), [](char L, char R) { return std::tolower(static_cast<unsigned char>(L)) == std::tolower(static_cast<unsigned char>(R)); });
	}

	int32 CountCells(int32 TextureSize, int32 TileSize, int32 Margin, int32 Spacing)
	{
		if (TileSize <= 0 || TextureSize <= 0)
		{
			return 0;
		}
		return std::max(0, (TextureSize - 2 * Margin + Spacing) / (TileSize + Spacing));
	}
} // namespace

const char* ToString(ETileCollision Collision)
{
	switch (Collision)
	{
	case ETileCollision::Full:    return "Full";
	case ETileCollision::Polygon: return "Polygon";
	case ETileCollision::None:
	default:                      return "None";
	}
}

ETileCollision ParseTileCollision(std::string_view Name, bool* bOutValid)
{
	ETileCollision Result = ETileCollision::None;
	bool           bValid = true;
	if (EqualsIgnoreCase(Name, "Full"))
	{
		Result = ETileCollision::Full;
	}
	else if (EqualsIgnoreCase(Name, "Polygon"))
	{
		Result = ETileCollision::Polygon;
	}
	else
	{
		bValid = EqualsIgnoreCase(Name, "None");
	}
	if (bOutValid != nullptr)
	{
		*bOutValid = bValid;
	}
	return Result;
}

bool FTileDefinition::HasTag(std::string_view Tag) const
{
	return std::find(Tags.begin(), Tags.end(), Tag) != Tags.end();
}

// ---- FTilesetAsset ---------------------------------------------------------------------------------------------------

int32 FTilesetAsset::GetColumns() const
{
	return CountCells(TextureWidth, TileWidth, Margin, Spacing);
}

int32 FTilesetAsset::GetRows() const
{
	return CountCells(TextureHeight, TileHeight, Margin, Spacing);
}

const FTileDefinition* FTilesetAsset::FindTile(int32 Id) const
{
	const auto It = std::lower_bound(Tiles.begin(), Tiles.end(), Id, [](const FTileDefinition& Tile, int32 Value) { return Tile.Id < Value; });
	return (It != Tiles.end() && It->Id == Id) ? &*It : nullptr;
}

FSpriteSlice FTilesetAsset::GetTileRect(int32 Id) const
{
	FSpriteSlice Rect;
	const int32  Columns = GetColumns();
	if (Id < 0 || Columns <= 0 || Id >= GetTileCount())
	{
		return Rect;
	}
	Rect.X = Margin + (Id % Columns) * (TileWidth + Spacing);
	Rect.Y = Margin + (Id / Columns) * (TileHeight + Spacing);
	Rect.W = TileWidth;
	Rect.H = TileHeight;
	return Rect;
}

FSpriteUvRect FTilesetAsset::ComputeTileUv(int32 Id) const
{
	const FSpriteSlice Rect = GetTileRect(Id);
	return Rect.W > 0 ? SpriteMath::ComputeUvRect(Rect, TextureWidth, TextureHeight) : FSpriteUvRect{};
}

void FTilesetAsset::Normalize()
{
	// 같은 Id는 뒤의 것 (stable_sort 후 뒤에서부터 고유)
	std::stable_sort(Tiles.begin(), Tiles.end(), [](const FTileDefinition& A, const FTileDefinition& B) { return A.Id < B.Id; });
	std::vector<FTileDefinition> Unique;
	Unique.reserve(Tiles.size());
	for (size_t Index = 0; Index < Tiles.size(); ++Index)
	{
		const bool bLastOfId = Index + 1 == Tiles.size() || Tiles[Index + 1].Id != Tiles[Index].Id;
		if (bLastOfId && !Tiles[Index].IsDefault())
		{
			Unique.push_back(std::move(Tiles[Index]));
		}
	}
	Tiles = std::move(Unique);
}

std::string FTilesetAsset::ToJsonString() const
{
	nlohmann::ordered_json Root;
	Root["Version"]       = Version;
	Root["Texture"]       = Texture;
	Root["TextureWidth"]  = TextureWidth;
	Root["TextureHeight"] = TextureHeight;
	Root["TileWidth"]     = TileWidth;
	Root["TileHeight"]    = TileHeight;
	Root["Margin"]        = Margin;
	Root["Spacing"]       = Spacing;
	Root["Columns"]       = GetColumns();
	Root["Rows"]          = GetRows();
	Root["UnitsPerPixel"] = UnitsPerPixel;
	Root["Filter"]        = ToString(Filter);
	nlohmann::ordered_json TileArray = nlohmann::ordered_json::array();
	for (const FTileDefinition& Tile : Tiles)
	{
		nlohmann::ordered_json Item;
		Item["Id"] = Tile.Id;
		if (Tile.Collision != ETileCollision::None)
		{
			Item["Collision"] = ToString(Tile.Collision);
		}
		if (!Tile.Points.empty())
		{
			nlohmann::ordered_json Points = nlohmann::ordered_json::array();
			for (const FVector2& Point : Tile.Points)
			{
				Points.push_back({ Point.X, Point.Y });
			}
			Item["Points"] = std::move(Points);
		}
		if (Tile.bOneWay)
		{
			Item["OneWay"] = true;
		}
		if (!Tile.Tags.empty())
		{
			Item["Tags"] = Tile.Tags;
		}
		if (!Tile.Animation.empty())
		{
			nlohmann::ordered_json Frames = nlohmann::ordered_json::array();
			for (const FTileAnimFrame& Frame : Tile.Animation)
			{
				Frames.push_back({ { "Tile", Frame.TileId }, { "Duration", Frame.Duration } });
			}
			Item["Animation"] = std::move(Frames);
		}
		TileArray.push_back(std::move(Item));
	}
	Root["Tiles"] = std::move(TileArray);
	return Root.dump(2);
}

bool FTilesetAsset::FromJsonString(std::string_view Json, FTilesetAsset& OutAsset, std::vector<std::string>* OutWarnings, std::string* OutError)
{
	FJson Root;
	if (!ParseRoot(Json, Version, "타일셋(.etileset)", Root, OutWarnings, OutError))
	{
		return false;
	}
	FTilesetAsset Asset;
	Asset.Texture       = GetString(Root, "Texture");
	Asset.TextureWidth  = std::max(0, GetInt(Root, "TextureWidth", 0));
	Asset.TextureHeight = std::max(0, GetInt(Root, "TextureHeight", 0));
	Asset.TileWidth     = GetInt(Root, "TileWidth", 16);
	Asset.TileHeight    = GetInt(Root, "TileHeight", 16);
	Asset.Margin        = std::max(0, GetInt(Root, "Margin", 0));
	Asset.Spacing       = std::max(0, GetInt(Root, "Spacing", 0));
	Asset.UnitsPerPixel = GetFloat(Root, "UnitsPerPixel", 1.0f);
	if (Asset.TileWidth <= 0 || Asset.TileHeight <= 0)
	{
		Warn(OutWarnings, std::format("타일 크기 {}x{}가 잘못되었습니다 (16x16 사용)", Asset.TileWidth, Asset.TileHeight));
		Asset.TileWidth  = 16;
		Asset.TileHeight = 16;
	}
	if (!(Asset.UnitsPerPixel > 0.0f))
	{
		Warn(OutWarnings, std::format("UnitsPerPixel {}은 양수여야 합니다 (1.0 사용)", Asset.UnitsPerPixel));
		Asset.UnitsPerPixel = 1.0f;
	}
	if (const std::string FilterName = GetString(Root, "Filter"); !FilterName.empty())
	{
		bool bValid  = true;
		Asset.Filter = ParseSpriteFilter(FilterName, &bValid);
		if (!bValid)
		{
			Warn(OutWarnings, std::format("모르는 Filter '{}' (Point 사용)", FilterName));
		}
	}
	if (Asset.Texture.empty())
	{
		Warn(OutWarnings, "Texture가 비었습니다");
	}
	const int32 TileCount = Asset.GetTileCount();
	if (const FJson* TileArray = GetArray(Root, "Tiles"))
	{
		for (const FJson& Item : *TileArray)
		{
			if (!Item.is_object())
			{
				continue;
			}
			FTileDefinition Tile;
			Tile.Id = GetInt(Item, "Id", -1);
			if (Tile.Id < 0 || (TileCount > 0 && Tile.Id >= TileCount))
			{
				Warn(OutWarnings, std::format("타일 Id {}가 범위 밖입니다 (타일 {}개)", Tile.Id, TileCount));
				if (Tile.Id < 0)
				{
					continue;
				}
			}
			if (const std::string CollisionName = GetString(Item, "Collision"); !CollisionName.empty())
			{
				bool bValid    = true;
				Tile.Collision = ParseTileCollision(CollisionName, &bValid);
				if (!bValid)
				{
					Warn(OutWarnings, std::format("타일 {}: 모르는 Collision '{}' (None)", Tile.Id, CollisionName));
				}
			}
			if (const FJson* Points = GetArray(Item, "Points"))
			{
				for (const FJson& Point : *Points)
				{
					if (Point.is_array() && Point.size() == 2 && Point[0].is_number() && Point[1].is_number())
					{
						Tile.Points.emplace_back(Point[0].get<float>(), Point[1].get<float>());
					}
				}
			}
			if (Tile.Collision == ETileCollision::Polygon && Tile.Points.size() < 3)
			{
				Warn(OutWarnings, std::format("타일 {}: 다각형 점이 3개 미만입니다 (충돌 없음)", Tile.Id));
			}
			Tile.bOneWay = GetBool(Item, "OneWay", false);
			if (const FJson* Tags = GetArray(Item, "Tags"))
			{
				for (const FJson& Tag : *Tags)
				{
					if (Tag.is_string() && !Tag.get<std::string>().empty())
					{
						Tile.Tags.push_back(Tag.get<std::string>());
					}
				}
			}
			if (const FJson* Frames = GetArray(Item, "Animation"))
			{
				for (const FJson& Frame : *Frames)
				{
					if (!Frame.is_object())
					{
						continue;
					}
					FTileAnimFrame AnimFrame;
					AnimFrame.TileId   = GetInt(Frame, "Tile", Tile.Id);
					AnimFrame.Duration = GetFloat(Frame, "Duration", 0.1f);
					if (!(AnimFrame.Duration > 0.0f))
					{
						Warn(OutWarnings, std::format("타일 {}: 애니메이션 프레임 길이는 양수여야 합니다 (0.1 사용)", Tile.Id));
						AnimFrame.Duration = 0.1f;
					}
					Tile.Animation.push_back(AnimFrame);
				}
			}
			Asset.Tiles.push_back(std::move(Tile));
		}
	}
	Asset.Normalize();
	OutAsset = std::move(Asset);
	return true;
}

#include "Scene/Sprite/SpriteAsset.h"

#include "Scene/Sprite/Sprite2DJson.h"

#include <algorithm>
#include <cctype>
#include <set>

using namespace Sprite2DJson;

namespace
{
	bool EqualsIgnoreCase(std::string_view A, std::string_view B)
	{
		return A.size() == B.size() &&
		       std::equal(A.begin(), A.end(), B.begin(), [](char L, char R) { return std::tolower(static_cast<unsigned char>(L)) == std::tolower(static_cast<unsigned char>(R)); });
	}
} // namespace

const char* ToString(ESpriteFilter Filter)
{
	switch (Filter)
	{
	case ESpriteFilter::Linear: return "Linear";
	case ESpriteFilter::Point:
	default:                    return "Point";
	}
}

ESpriteFilter ParseSpriteFilter(std::string_view Name, bool* bOutValid)
{
	const bool bLinear = EqualsIgnoreCase(Name, "Linear");
	const bool bPoint  = EqualsIgnoreCase(Name, "Point");
	if (bOutValid != nullptr)
	{
		*bOutValid = bLinear || bPoint;
	}
	return bLinear ? ESpriteFilter::Linear : ESpriteFilter::Point;
}

// ---- FSpriteAsset ----------------------------------------------------------------------------------------------------

int32 FSpriteAsset::FindSlice(std::string_view Name) const
{
	if (Name.empty())
	{
		return Slices.empty() ? -1 : 0;
	}
	for (size_t Index = 0; Index < Slices.size(); ++Index)
	{
		if (Slices[Index].Name == Name)
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

std::vector<std::string> FSpriteAsset::GetSliceNames() const
{
	std::vector<std::string> Names;
	Names.reserve(Slices.size());
	for (const FSpriteSlice& Slice : Slices)
	{
		Names.push_back(Slice.Name);
	}
	return Names;
}

std::string FSpriteAsset::ToJsonString() const
{
	nlohmann::ordered_json Root;
	Root["Version"]       = Version;
	Root["Texture"]       = Texture;
	Root["TextureWidth"]  = TextureWidth;
	Root["TextureHeight"] = TextureHeight;
	Root["UnitsPerPixel"] = UnitsPerPixel;
	Root["Filter"]        = ToString(Filter);
	nlohmann::ordered_json SliceArray = nlohmann::ordered_json::array();
	for (const FSpriteSlice& Slice : Slices)
	{
		nlohmann::ordered_json Item;
		Item["Name"] = Slice.Name;
		Item["X"]    = Slice.X;
		Item["Y"]    = Slice.Y;
		Item["W"]    = Slice.W;
		Item["H"]    = Slice.H;
		if (Slice.Pivot != FVector2(0.5f, 0.5f))
		{
			Item["Pivot"] = { Slice.Pivot.X, Slice.Pivot.Y };
		}
		if (Slice.HasBorder())
		{
			Item["Border"] = { Slice.BorderLeft, Slice.BorderTop, Slice.BorderRight, Slice.BorderBottom };
		}
		SliceArray.push_back(std::move(Item));
	}
	Root["Slices"] = std::move(SliceArray);
	return Root.dump(2);
}

bool FSpriteAsset::FromJsonString(std::string_view Json, FSpriteAsset& OutAsset, std::vector<std::string>* OutWarnings, std::string* OutError)
{
	FJson Root;
	if (!ParseRoot(Json, Version, "스프라이트(.esprite)", Root, OutWarnings, OutError))
	{
		return false;
	}
	FSpriteAsset Asset;
	Asset.Texture       = GetString(Root, "Texture");
	Asset.TextureWidth  = std::max(0, GetInt(Root, "TextureWidth", 0));
	Asset.TextureHeight = std::max(0, GetInt(Root, "TextureHeight", 0));
	Asset.UnitsPerPixel = GetFloat(Root, "UnitsPerPixel", 1.0f);
	if (!(Asset.UnitsPerPixel > 0.0f))
	{
		Warn(OutWarnings, std::format("UnitsPerPixel {}은 양수여야 합니다 (1.0 사용)", Asset.UnitsPerPixel));
		Asset.UnitsPerPixel = 1.0f;
	}
	bool bValidFilter = true;
	if (const std::string FilterName = GetString(Root, "Filter"); !FilterName.empty())
	{
		Asset.Filter = ParseSpriteFilter(FilterName, &bValidFilter);
		if (!bValidFilter)
		{
			Warn(OutWarnings, std::format("모르는 Filter '{}' (Point 사용)", FilterName));
		}
	}
	if (Asset.Texture.empty())
	{
		Warn(OutWarnings, "Texture가 비었습니다");
	}

	std::set<std::string> SeenNames;
	if (const FJson* SliceArray = GetArray(Root, "Slices"))
	{
		Asset.Slices.reserve(SliceArray->size());
		for (const FJson& Item : *SliceArray)
		{
			if (!Item.is_object())
			{
				Warn(OutWarnings, "Slices 항목이 객체가 아닙니다 (무시)");
				continue;
			}
			FSpriteSlice Slice;
			Slice.Name  = GetString(Item, "Name");
			Slice.X     = GetInt(Item, "X", 0);
			Slice.Y     = GetInt(Item, "Y", 0);
			Slice.W     = GetInt(Item, "W", 0);
			Slice.H     = GetInt(Item, "H", 0);
			Slice.Pivot = GetVector2(Item, "Pivot", FVector2(0.5f, 0.5f));
			if (const FJson* Border = GetArray(Item, "Border"); Border != nullptr && Border->size() == 4)
			{
				int32* Targets[4] = { &Slice.BorderLeft, &Slice.BorderTop, &Slice.BorderRight, &Slice.BorderBottom };
				for (size_t Side = 0; Side < 4; ++Side)
				{
					*Targets[Side] = (*Border)[Side].is_number() ? std::max(0, static_cast<int32>((*Border)[Side].get<double>())) : 0;
				}
			}
			if (Slice.Name.empty())
			{
				Slice.Name = std::format("Slice_{}", Asset.Slices.size());
				Warn(OutWarnings, std::format("이름 없는 슬라이스 → '{}'", Slice.Name));
			}
			if (!SeenNames.insert(Slice.Name).second)
			{
				Warn(OutWarnings, std::format("슬라이스 이름 중복 '{}' (이름으로는 첫 번째만 찾는다)", Slice.Name));
			}
			if (Slice.W <= 0 || Slice.H <= 0 || Slice.X < 0 || Slice.Y < 0)
			{
				Warn(OutWarnings, std::format("슬라이스 '{}' 사각형이 잘못되었습니다 ({}, {}, {}x{})", Slice.Name, Slice.X, Slice.Y, Slice.W, Slice.H));
				Slice.X = std::max(0, Slice.X);
				Slice.Y = std::max(0, Slice.Y);
				Slice.W = std::max(0, Slice.W);
				Slice.H = std::max(0, Slice.H);
			}
			else if (Asset.TextureWidth > 0 && Asset.TextureHeight > 0 && (Slice.X + Slice.W > Asset.TextureWidth || Slice.Y + Slice.H > Asset.TextureHeight))
			{
				Warn(OutWarnings, std::format("슬라이스 '{}'가 텍스처({}x{}) 밖으로 나갑니다", Slice.Name, Asset.TextureWidth, Asset.TextureHeight));
			}
			Asset.Slices.push_back(std::move(Slice));
		}
	}
	OutAsset = std::move(Asset);
	return true;
}

// ---- SpriteMath ------------------------------------------------------------------------------------------------------

std::vector<FSpriteSlice> SpriteMath::SliceGrid(int32 TextureWidth, int32 TextureHeight, int32 CellWidth, int32 CellHeight, int32 Margin, int32 Spacing,
                                                std::string_view NamePrefix)
{
	std::vector<FSpriteSlice> Result;
	if (CellWidth <= 0 || CellHeight <= 0 || Margin < 0 || Spacing < 0)
	{
		return Result;
	}
	const int32 Columns = std::max(0, (TextureWidth - 2 * Margin + Spacing) / (CellWidth + Spacing));
	const int32 Rows    = std::max(0, (TextureHeight - 2 * Margin + Spacing) / (CellHeight + Spacing));
	Result.reserve(static_cast<size_t>(Columns) * static_cast<size_t>(Rows));
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		for (int32 Column = 0; Column < Columns; ++Column)
		{
			FSpriteSlice& Slice = Result.emplace_back();
			Slice.Name          = std::format("{}{}", NamePrefix, Row * Columns + Column);
			Slice.X             = Margin + Column * (CellWidth + Spacing);
			Slice.Y             = Margin + Row * (CellHeight + Spacing);
			Slice.W             = CellWidth;
			Slice.H             = CellHeight;
		}
	}
	return Result;
}

FSpriteUvRect SpriteMath::ComputeUvRect(const FSpriteSlice& Slice, int32 TextureWidth, int32 TextureHeight)
{
	if (TextureWidth <= 0 || TextureHeight <= 0)
	{
		return {};
	}
	const float InvWidth  = 1.0f / static_cast<float>(TextureWidth);
	const float InvHeight = 1.0f / static_cast<float>(TextureHeight);
	return { static_cast<float>(Slice.X) * InvWidth, static_cast<float>(Slice.Y) * InvHeight, static_cast<float>(Slice.X + Slice.W) * InvWidth,
		     static_cast<float>(Slice.Y + Slice.H) * InvHeight };
}

FVector2 SpriteMath::ComputeSize(const FSpriteSlice& Slice, float UnitsPerPixel, const FVector2& Size)
{
	const FVector2 PixelSize(static_cast<float>(Slice.W) * UnitsPerPixel, static_cast<float>(Slice.H) * UnitsPerPixel);
	const bool     bHasX = Size.X > 0.0f;
	const bool     bHasY = Size.Y > 0.0f;
	if (bHasX && bHasY)
	{
		return Size;
	}
	if (bHasX)
	{
		return FVector2(Size.X, PixelSize.X > 0.0f ? Size.X * PixelSize.Y / PixelSize.X : 0.0f);
	}
	if (bHasY)
	{
		return FVector2(PixelSize.Y > 0.0f ? Size.Y * PixelSize.X / PixelSize.Y : 0.0f, Size.Y);
	}
	return PixelSize;
}

FSpriteQuad SpriteMath::ComputeQuad(const FSpriteSlice& Slice, int32 TextureWidth, int32 TextureHeight, float UnitsPerPixel, const FVector2& Size, bool bFlipX,
                                    bool bFlipY)
{
	const FVector2      Extent = ComputeSize(Slice, UnitsPerPixel, Size);
	const FSpriteUvRect Uv     = ComputeUvRect(Slice, TextureWidth, TextureHeight);

	// 피벗 기준 가장자리 (반전 전)
	float Left   = -Slice.Pivot.X * Extent.X;
	float Right  = (1.0f - Slice.Pivot.X) * Extent.X;
	float Bottom = -Slice.Pivot.Y * Extent.Y;
	float Top    = (1.0f - Slice.Pivot.Y) * Extent.Y;
	// 이미지 왼쪽/오른쪽 U, 아래/위 V (이미지 V는 아래로 + → 사각형 위 = V0)
	float ULeft = Uv.U0, URight = Uv.U1;
	float VBottom = Uv.V1, VTop = Uv.V0;
	if (bFlipX)
	{
		// 피벗을 지나는 세로축 거울: 가장자리 위치를 뒤집고 U를 맞바꾼다 (정점 공간 순서는 그대로 → 와인딩 불변)
		const float OldLeft = Left;
		Left                = -Right;
		Right               = -OldLeft;
		std::swap(ULeft, URight);
	}
	if (bFlipY)
	{
		const float OldBottom = Bottom;
		Bottom                = -Top;
		Top                   = -OldBottom;
		std::swap(VBottom, VTop);
	}

	FSpriteQuad Quad;
	Quad.Positions[0] = FVector2(Left, Bottom);
	Quad.Positions[1] = FVector2(Right, Bottom);
	Quad.Positions[2] = FVector2(Right, Top);
	Quad.Positions[3] = FVector2(Left, Top);
	Quad.Uvs[0]       = FVector2(ULeft, VBottom);
	Quad.Uvs[1]       = FVector2(URight, VBottom);
	Quad.Uvs[2]       = FVector2(URight, VTop);
	Quad.Uvs[3]       = FVector2(ULeft, VTop);
	return Quad;
}

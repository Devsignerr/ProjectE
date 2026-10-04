#include "Scene/Sprite/TilemapCollision.h"

#include "Scene/Sprite/TilemapData.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <algorithm>
#include <set>
#include <utility>

namespace
{
	using FCellSet = std::set<std::pair<int32, int32>>; // (Y, X)

	// 탐욕 사각형 병합 (머리 주석 규칙)
	void MergeBoxes(FCellSet& Cells, const FVector2& CellSize, std::vector<FTileCollisionBox>& Out)
	{
		while (!Cells.empty())
		{
			const auto [StartY, StartX] = *Cells.begin();
			int32 Width                 = 1;
			while (Cells.contains({ StartY, StartX + Width }))
			{
				++Width;
			}
			int32 Height = 1;
			while (true)
			{
				bool bRowFull = true;
				for (int32 X = StartX; X < StartX + Width && bRowFull; ++X)
				{
					bRowFull = Cells.contains({ StartY + Height, X });
				}
				if (!bRowFull)
				{
					break;
				}
				++Height;
			}
			for (int32 Y = StartY; Y < StartY + Height; ++Y)
			{
				for (int32 X = StartX; X < StartX + Width; ++X)
				{
					Cells.erase({ Y, X });
				}
			}
			Out.push_back({ TilemapMath::CellToLocal(StartX, StartY, CellSize), TilemapMath::CellToLocal(StartX + Width, StartY + Height, CellSize) });
		}
	}
} // namespace

FVector2 TilemapCollision::ResolveCellSize(const FTilesetAsset& Tileset, const FVector2& CellSize)
{
	const FVector2 TileSize = Tileset.GetTileSizeInUnits();
	return FVector2(CellSize.X > 0.0f ? CellSize.X : TileSize.X, CellSize.Y > 0.0f ? CellSize.Y : TileSize.Y);
}

FTilemapCollisionShapes TilemapCollision::BuildShapes(const FTilemapData& Data, const FTilesetAsset& Tileset, const FVector2& CellSize)
{
	FTilemapCollisionShapes Shapes;
	const FVector2          Size = ResolveCellSize(Tileset, CellSize);
	if (!(Size.X > 0.0f) || !(Size.Y > 0.0f) || Tileset.TileWidth <= 0 || Tileset.TileHeight <= 0)
	{
		return Shapes;
	}
	const float InvTileWidth  = 1.0f / static_cast<float>(Tileset.TileWidth);
	const float InvTileHeight = 1.0f / static_cast<float>(Tileset.TileHeight);

	FCellSet FullCells;
	FCellSet OneWayCells;
	Data.ForEachCell([&](int32 X, int32 Y, uint32 Cell) {
		const FTileDefinition* Tile = Tileset.FindTile(TileCell::GetTileId(Cell));
		if (Tile == nullptr)
		{
			return;
		}
		if (Tile->Collision == ETileCollision::Full)
		{
			(Tile->bOneWay ? OneWayCells : FullCells).insert({ Y, X });
			return;
		}
		if (Tile->Collision != ETileCollision::Polygon || Tile->Points.size() < 3)
		{
			return;
		}
		const uint32          Flags  = TileCell::GetFlags(Cell);
		const FVector2        Origin = TilemapMath::CellToLocal(X, Y, Size);
		FTileCollisionPolygon Polygon;
		Polygon.Points.reserve(Tile->Points.size());
		for (const FVector2& Pixel : Tile->Points)
		{
			const FVector2 Normalized(Pixel.X * InvTileWidth - 0.5f, 0.5f - Pixel.Y * InvTileHeight);
			const FVector2 Transformed = TilemapMath::TransformTileUv(Normalized, Flags);
			Polygon.Points.emplace_back(Origin.X + (Transformed.X + 0.5f) * Size.X, Origin.Y + (Transformed.Y + 0.5f) * Size.Y);
		}
		if (TilemapMath::FlipsWinding(Flags))
		{
			std::reverse(Polygon.Points.begin(), Polygon.Points.end());
		}
		(Tile->bOneWay ? Shapes.OneWayPolygons : Shapes.Polygons).push_back(std::move(Polygon));
	});
	MergeBoxes(FullCells, Size, Shapes.Boxes);
	MergeBoxes(OneWayCells, Size, Shapes.OneWayBoxes);
	return Shapes;
}

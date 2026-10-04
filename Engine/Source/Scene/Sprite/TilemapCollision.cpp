#include "Scene/Sprite/TilemapCollision.h"

#include "Scene/Sprite/TilemapData.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
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
	// 방향 번호: 0 = +X, 1 = +Y, 2 = -X, 3 = -Y (왼쪽 회전 = +1)
	constexpr int32 DirX[4] = { 1, 0, -1, 0 };
	constexpr int32 DirY[4] = { 0, 1, 0, -1 };

	using FEdgeKey = std::tuple<int32, int32, int32>; // (시작 Y, 시작 X, 방향)

	bool IsCoordLess(const FTileCoord& A, const FTileCoord& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; }

	// 공선 점 제거 (직교 고리: 들어온 방향 == 나가는 방향이면 지운다)
	std::vector<FTileCoord> RemoveCollinear(const std::vector<FTileCoord>& Loop)
	{
		std::vector<FTileCoord> Result;
		const size_t            Count = Loop.size();
		Result.reserve(Count);
		for (size_t Index = 0; Index < Count; ++Index)
		{
			const FTileCoord& Previous = Loop[(Index + Count - 1) % Count];
			const FTileCoord& Current  = Loop[Index];
			const FTileCoord& Next     = Loop[(Index + 1) % Count];
			const int64       Cross    = static_cast<int64>(Current.X - Previous.X) * (Next.Y - Current.Y) -
			                    static_cast<int64>(Current.Y - Previous.Y) * (Next.X - Current.X);
			if (Cross != 0)
			{
				Result.push_back(Current);
			}
		}
		// (Y, X)가 가장 작은 꼭짓점부터
		const auto Min = std::min_element(Result.begin(), Result.end(), IsCoordLess);
		std::rotate(Result.begin(), Min, Result.end());
		return Result;
	}

	int64 TwiceSignedArea(const std::vector<FTileCoord>& Loop)
	{
		int64 Sum = 0;
		for (size_t Index = 0; Index < Loop.size(); ++Index)
		{
			const FTileCoord& A = Loop[Index];
			const FTileCoord& B = Loop[(Index + 1) % Loop.size()];
			Sum += static_cast<int64>(A.X) * B.Y - static_cast<int64>(B.X) * A.Y;
		}
		return Sum;
	}
} // namespace

std::vector<std::vector<FTileCoord>> TilemapCollision::TraceOutlines(const std::vector<FTileCoord>& Cells)
{
	std::set<std::pair<int32, int32>> Filled; // (Y, X)
	for (const FTileCoord& Cell : Cells)
	{
		Filled.insert({ Cell.Y, Cell.X });
	}
	const auto IsFilled = [&](int32 X, int32 Y) { return Filled.contains({ Y, X }); };

	// 경계 모서리 (채운 칸이 진행 방향 왼쪽)
	std::set<FEdgeKey> Edges;
	for (const auto& [Y, X] : Filled)
	{
		if (!IsFilled(X, Y - 1))
		{
			Edges.insert({ Y, X, 0 }); // 아랫변 (x, y) → (x+1, y)
		}
		if (!IsFilled(X + 1, Y))
		{
			Edges.insert({ Y, X + 1, 1 }); // 오른변 (x+1, y) → (x+1, y+1)
		}
		if (!IsFilled(X, Y + 1))
		{
			Edges.insert({ Y + 1, X + 1, 2 }); // 윗변 (x+1, y+1) → (x, y+1)
		}
		if (!IsFilled(X - 1, Y))
		{
			Edges.insert({ Y + 1, X, 3 }); // 왼변 (x, y+1) → (x, y)
		}
	}

	std::vector<std::vector<FTileCoord>> Loops;
	while (!Edges.empty())
	{
		// 고리 하나 따라가기 (시작 모서리는 닫힐 때 지운다)
		const FEdgeKey          Start = *Edges.begin();
		std::vector<FTileCoord> Path;
		int32                   Y   = std::get<0>(Start);
		int32                   X   = std::get<1>(Start);
		int32                   Dir = std::get<2>(Start);
		while (true)
		{
			Path.push_back({ X, Y });
			if (FEdgeKey{ Y, X, Dir } != Start)
			{
				Edges.erase({ Y, X, Dir });
			}
			X += DirX[Dir];
			Y += DirY[Dir];
			int32 NextDir = -1;
			for (const int32 Turn : { 1, 0, 3 }) // 왼쪽 → 직진 → 오른쪽
			{
				const int32 Candidate = (Dir + Turn) & 3;
				if (Edges.contains({ Y, X, Candidate }))
				{
					NextDir = Candidate;
					break;
				}
			}
			if (NextDir < 0 || FEdgeKey{ Y, X, NextDir } == Start)
			{
				Edges.erase(Start);
				break; // 닫힘 (모서리 집합이 닫혀 있으므로 NextDir < 0은 생기지 않는다 — 방어)
			}
			Dir = NextDir;
		}

		// 같은 꼭짓점을 다시 지나면 그 자리에서 고리를 나눈다
		std::vector<FTileCoord>          Stack;
		std::map<std::pair<int32, int32>, size_t> Positions; // (Y, X) → Stack 위치
		const auto EmitLoop = [&](std::vector<FTileCoord> Loop) {
			if (Loop.size() >= 4)
			{
				Loops.push_back(RemoveCollinear(Loop));
			}
		};
		for (const FTileCoord& Vertex : Path)
		{
			const auto Found = Positions.find({ Vertex.Y, Vertex.X });
			if (Found == Positions.end())
			{
				Positions[{ Vertex.Y, Vertex.X }] = Stack.size();
				Stack.push_back(Vertex);
				continue;
			}
			const size_t            From = Found->second;
			std::vector<FTileCoord> Loop(Stack.begin() + static_cast<std::ptrdiff_t>(From), Stack.end());
			for (size_t Index = From + 1; Index < Stack.size(); ++Index)
			{
				Positions.erase({ Stack[Index].Y, Stack[Index].X });
			}
			Stack.resize(From + 1);
			EmitLoop(std::move(Loop));
		}
		EmitLoop(std::move(Stack));
	}

	std::sort(Loops.begin(), Loops.end(), [](const std::vector<FTileCoord>& A, const std::vector<FTileCoord>& B) {
		return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), IsCoordLess);
	});
	return Loops;
}

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

	FCellSet                FullCells;
	FCellSet                OneWayCells;
	std::vector<FTileCoord> FullList;
	Data.ForEachCell([&](int32 X, int32 Y, uint32 Cell) {
		const FTileDefinition* Tile = Tileset.FindTile(TileCell::GetTileId(Cell));
		if (Tile == nullptr)
		{
			return;
		}
		if (Tile->Collision == ETileCollision::Full)
		{
			(Tile->bOneWay ? OneWayCells : FullCells).insert({ Y, X });
			if (!Tile->bOneWay)
			{
				FullList.push_back({ X, Y });
			}
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
	// 외곽선 (물리 체인)
	for (const std::vector<FTileCoord>& Loop : TraceOutlines(FullList))
	{
		FTileCollisionOutline Outline;
		Outline.bHole = TwiceSignedArea(Loop) < 0;
		Outline.Points.reserve(Loop.size());
		for (const FTileCoord& Vertex : Loop)
		{
			Outline.Points.push_back(TilemapMath::CellToLocal(Vertex.X, Vertex.Y, Size));
		}
		Shapes.Outlines.push_back(std::move(Outline));
	}
	// 원웨이 윗변: 위 칸이 원웨이 Full이 아닌 칸을 행마다 X로 이어 붙인다 (OneWayCells는 (Y, X) 순)
	for (auto It = OneWayCells.begin(); It != OneWayCells.end();)
	{
		const auto [Y, StartX] = *It;
		if (OneWayCells.contains({ Y + 1, StartX }))
		{
			++It;
			continue;
		}
		int32 EndX = StartX + 1;
		++It;
		while (It != OneWayCells.end() && It->first == Y && It->second == EndX && !OneWayCells.contains({ Y + 1, EndX }))
		{
			++EndX;
			++It;
		}
		Shapes.OneWaySegments.push_back({ TilemapMath::CellToLocal(EndX, Y + 1, Size), TilemapMath::CellToLocal(StartX, Y + 1, Size) });
	}
	MergeBoxes(FullCells, Size, Shapes.Boxes);
	MergeBoxes(OneWayCells, Size, Shapes.OneWayBoxes);
	return Shapes;
}

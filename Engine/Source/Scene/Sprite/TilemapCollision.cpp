#include "Scene/Sprite/TilemapCollision.h"

#include "Scene/Sprite/TilemapData.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <algorithm>
#include <cmath>
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

	// ---- TraceSolidOutlines (칸 단위 다각형 합집합 외곽선)
	constexpr double SolidQuantize = 4096.0; // 1/4096칸

	struct FQPoint
	{
		int64 X = 0;
		int64 Y = 0;
		bool  operator==(const FQPoint& Other) const = default;
		bool  operator<(const FQPoint& Other) const { return Y != Other.Y ? Y < Other.Y : X < Other.X; }
	};

	int64 Cross(const FQPoint& A, const FQPoint& B, const FQPoint& C) // (B - A) × (C - B)
	{
		return (B.X - A.X) * (C.Y - B.Y) - (B.Y - A.Y) * (C.X - B.X);
	}

	// +X부터 반시계 각 [0, 2π)
	double DirectionAngle(const FQPoint& From, const FQPoint& To)
	{
		const double Angle = std::atan2(static_cast<double>(To.Y - From.Y), static_cast<double>(To.X - From.X));
		return Angle < 0.0 ? Angle + 2.0 * 3.14159265358979323846 : Angle;
	}

	// P가 선분 A-B 위(끝점 제외)에 있는가
	bool IsStrictlyOnSegment(const FQPoint& A, const FQPoint& B, const FQPoint& P)
	{
		if (P == A || P == B || (B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X) != 0)
		{
			return false;
		}
		return std::min(A.X, B.X) <= P.X && P.X <= std::max(A.X, B.X) && std::min(A.Y, B.Y) <= P.Y && P.Y <= std::max(A.Y, B.Y);
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

std::vector<std::vector<FVector2>> TilemapCollision::TraceSolidOutlines(const std::vector<std::vector<FVector2>>& Polygons)
{
	// 1) 다각형 → 반시계 정수 좌표 (연속 중복 점·넓이 0 제외)
	std::vector<std::vector<FQPoint>> Rings;
	Rings.reserve(Polygons.size());
	for (const std::vector<FVector2>& Polygon : Polygons)
	{
		std::vector<FQPoint> Ring;
		Ring.reserve(Polygon.size());
		for (const FVector2& Point : Polygon)
		{
			if (!std::isfinite(Point.X) || !std::isfinite(Point.Y))
			{
				Ring.clear();
				break;
			}
			const FQPoint Q{ std::llround(static_cast<double>(Point.X) * SolidQuantize), std::llround(static_cast<double>(Point.Y) * SolidQuantize) };
			if (Ring.empty() || !(Ring.back() == Q))
			{
				Ring.push_back(Q);
			}
		}
		while (Ring.size() > 1 && Ring.front() == Ring.back())
		{
			Ring.pop_back();
		}
		if (Ring.size() < 3)
		{
			continue;
		}
		int64 Area = 0;
		for (size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const FQPoint& A = Ring[Index];
			const FQPoint& B = Ring[(Index + 1) % Ring.size()];
			Area += (A.X - Ring[0].X) * (B.Y - Ring[0].Y) - (B.X - Ring[0].X) * (A.Y - Ring[0].Y); // 첫 점 기준 (큰 좌표 넘침 방지)
		}
		if (Area == 0)
		{
			continue;
		}
		if (Area < 0)
		{
			std::reverse(Ring.begin(), Ring.end());
		}
		Rings.push_back(std::move(Ring));
	}

	// 2) 변 모으기 + 다른 꼭짓점에서 나누기 (꼭짓점은 칸별로 찾는다 — 변은 자기 칸 안이라 이웃 3x3 칸만 보면 된다)
	const int64                                  CellUnit = static_cast<int64>(SolidQuantize);
	const auto                                   CellOf   = [&](int64 V) { return V >= 0 ? V / CellUnit : -((-V + CellUnit - 1) / CellUnit); };
	std::map<std::pair<int64, int64>, std::set<FQPoint>> VerticesByCell; // (칸 Y, 칸 X) → 꼭짓점
	for (const std::vector<FQPoint>& Ring : Rings)
	{
		for (const FQPoint& Point : Ring)
		{
			VerticesByCell[{ CellOf(Point.Y), CellOf(Point.X) }].insert(Point);
		}
	}
	std::map<std::pair<FQPoint, FQPoint>, int32> EdgeCounts; // (시작, 끝) → 개수
	for (const std::vector<FQPoint>& Ring : Rings)
	{
		for (size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const FQPoint&       A = Ring[Index];
			const FQPoint&       B = Ring[(Index + 1) % Ring.size()];
			std::vector<FQPoint> Splits;
			const int64          MinCellX = CellOf(std::min(A.X, B.X)) - 1, MaxCellX = CellOf(std::max(A.X, B.X)) + 1;
			const int64          MinCellY = CellOf(std::min(A.Y, B.Y)) - 1, MaxCellY = CellOf(std::max(A.Y, B.Y)) + 1;
			for (int64 CellY = MinCellY; CellY <= MaxCellY; ++CellY)
			{
				for (int64 CellX = MinCellX; CellX <= MaxCellX; ++CellX)
				{
					const auto Found = VerticesByCell.find({ CellY, CellX });
					if (Found == VerticesByCell.end())
					{
						continue;
					}
					for (const FQPoint& Point : Found->second)
					{
						if (IsStrictlyOnSegment(A, B, Point))
						{
							Splits.push_back(Point);
						}
					}
				}
			}
			// A에서 가까운 순
			std::sort(Splits.begin(), Splits.end(), [&](const FQPoint& L, const FQPoint& R) {
				return std::abs(L.X - A.X) + std::abs(L.Y - A.Y) < std::abs(R.X - A.X) + std::abs(R.Y - A.Y);
			});
			Splits.erase(std::unique(Splits.begin(), Splits.end()), Splits.end());
			FQPoint From = A;
			for (const FQPoint& Point : Splits)
			{
				++EdgeCounts[{ From, Point }];
				From = Point;
			}
			++EdgeCounts[{ From, B }];
		}
	}

	// 3) 반대 방향으로 겹친 변 쌍 지우기 (맞닿은 안쪽 경계)
	std::map<FQPoint, std::vector<FQPoint>> Outgoing; // 시작 → 끝들 (남은 변)
	for (const auto& [Edge, Count] : EdgeCounts)
	{
		const auto  Reverse   = EdgeCounts.find({ Edge.second, Edge.first });
		const int32 Remaining = Count - (Reverse != EdgeCounts.end() ? std::min(Count, Reverse->second) : 0);
		for (int32 Index = 0; Index < Remaining; ++Index)
		{
			Outgoing[Edge.first].push_back(Edge.second);
		}
	}
	const auto RemoveEdge = [&](const FQPoint& From, const FQPoint& To) {
		const auto Found = Outgoing.find(From);
		if (Found == Outgoing.end())
		{
			return;
		}
		const auto It = std::find(Found->second.begin(), Found->second.end(), To);
		if (It != Found->second.end())
		{
			Found->second.erase(It);
		}
		if (Found->second.empty())
		{
			Outgoing.erase(Found);
		}
	};

	// 4) 고리 따라가기 (가장 왼쪽으로 꺾는 변) + 같은 꼭짓점 재방문에서 나누기
	std::vector<std::vector<FQPoint>> Loops;
	const auto EmitLoop = [&](const std::vector<FQPoint>& Raw) {
		// 공선 점·되돌아가는 점 제거
		std::vector<FQPoint> Loop = Raw;
		bool                 bChanged = true;
		while (bChanged && Loop.size() >= 3)
		{
			bChanged = false;
			for (size_t Index = 0; Index < Loop.size() && Loop.size() >= 3; ++Index)
			{
				const size_t   Count    = Loop.size();
				const FQPoint& Previous = Loop[(Index + Count - 1) % Count];
				const FQPoint& Current  = Loop[Index];
				const FQPoint& Next     = Loop[(Index + 1) % Count];
				if (Current == Previous || Cross(Previous, Current, Next) == 0)
				{
					Loop.erase(Loop.begin() + static_cast<std::ptrdiff_t>(Index));
					bChanged = true;
					--Index;
				}
			}
		}
		if (Loop.size() >= 3)
		{
			std::rotate(Loop.begin(), std::min_element(Loop.begin(), Loop.end()), Loop.end());
			Loops.push_back(std::move(Loop));
		}
	};
	while (!Outgoing.empty())
	{
		// 시작: (시작 Y, X)가 가장 작은 꼭짓점의 변 중 각이 가장 작은 것
		const FQPoint StartFrom = Outgoing.begin()->first;
		FQPoint       StartTo   = Outgoing.begin()->second.front();
		for (const FQPoint& To : Outgoing.begin()->second)
		{
			if (DirectionAngle(StartFrom, To) < DirectionAngle(StartFrom, StartTo))
			{
				StartTo = To;
			}
		}
		std::vector<FQPoint> Path;
		FQPoint              From = StartFrom;
		FQPoint              To   = StartTo;
		while (true)
		{
			Path.push_back(From);
			RemoveEdge(From, To);
			if (To == StartFrom)
			{
				break;
			}
			const auto Found = Outgoing.find(To);
			if (Found == Outgoing.end())
			{
				break; // 열린 경로 (겹친 입력 — 방어)
			}
			// 들어온 방향 기준으로 가장 왼쪽(반시계로 가장 많이) 꺾는 변. 되돌아가는 변은 마지막 후보
			const double Incoming = DirectionAngle(From, To);
			FQPoint      Best     = Found->second.front();
			double       BestTurn = -10.0;
			for (const FQPoint& Candidate : Found->second)
			{
				double Turn = DirectionAngle(To, Candidate) - Incoming; // (-2π, 2π)
				while (Turn <= -3.14159265358979323846)
				{
					Turn += 2.0 * 3.14159265358979323846;
				}
				while (Turn > 3.14159265358979323846)
				{
					Turn -= 2.0 * 3.14159265358979323846;
				}
				if (Candidate == From)
				{
					Turn = -9.0;
				}
				if (Turn > BestTurn)
				{
					BestTurn = Turn;
					Best     = Candidate;
				}
			}
			From = To;
			To   = Best;
		}
		// 같은 꼭짓점 재방문에서 나누기 (TraceOutlines와 같은 규칙)
		std::vector<FQPoint>      Stack;
		std::map<FQPoint, size_t> Positions;
		for (const FQPoint& Vertex : Path)
		{
			const auto Found = Positions.find(Vertex);
			if (Found == Positions.end())
			{
				Positions[Vertex] = Stack.size();
				Stack.push_back(Vertex);
				continue;
			}
			const size_t         FromIndex = Found->second;
			std::vector<FQPoint> Loop(Stack.begin() + static_cast<std::ptrdiff_t>(FromIndex), Stack.end());
			for (size_t Index = FromIndex + 1; Index < Stack.size(); ++Index)
			{
				Positions.erase(Stack[Index]);
			}
			Stack.resize(FromIndex + 1);
			EmitLoop(Loop);
		}
		EmitLoop(Stack);
	}
	std::sort(Loops.begin(), Loops.end(), [](const std::vector<FQPoint>& A, const std::vector<FQPoint>& B) {
		return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end());
	});

	// 5) 칸 좌표로 (3점 고리는 첫 변 가운데 점을 더한다)
	std::vector<std::vector<FVector2>> Result;
	Result.reserve(Loops.size());
	for (const std::vector<FQPoint>& Loop : Loops)
	{
		std::vector<FVector2> Points;
		Points.reserve(Loop.size() + 1);
		for (size_t Index = 0; Index < Loop.size(); ++Index)
		{
			Points.emplace_back(static_cast<float>(static_cast<double>(Loop[Index].X) / SolidQuantize),
			                    static_cast<float>(static_cast<double>(Loop[Index].Y) / SolidQuantize));
			if (Index == 0 && Loop.size() == 3)
			{
				Points.emplace_back(static_cast<float>(static_cast<double>(Loop[0].X + Loop[1].X) * 0.5 / SolidQuantize),
				                    static_cast<float>(static_cast<double>(Loop[0].Y + Loop[1].Y) * 0.5 / SolidQuantize));
			}
		}
		Result.push_back(std::move(Points));
	}
	return Result;
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

	FCellSet                           FullCells;
	FCellSet                           OneWayCells;
	std::vector<std::vector<FVector2>> SolidPolygons; // 칸 좌표 (외곽선 합집합 — Full 칸 사각형 + 원웨이 아닌 다각형 타일)
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
				const float CellX = static_cast<float>(X), CellY = static_cast<float>(Y);
				SolidPolygons.push_back({ FVector2(CellX, CellY), FVector2(CellX + 1.0f, CellY), FVector2(CellX + 1.0f, CellY + 1.0f), FVector2(CellX, CellY + 1.0f) });
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
		std::vector<FVector2> CellPoints;
		CellPoints.reserve(Tile->Points.size());
		for (const FVector2& Pixel : Tile->Points)
		{
			const FVector2 Normalized(Pixel.X * InvTileWidth - 0.5f, 0.5f - Pixel.Y * InvTileHeight);
			const FVector2 Transformed = TilemapMath::TransformTileUv(Normalized, Flags);
			Polygon.Points.emplace_back(Origin.X + (Transformed.X + 0.5f) * Size.X, Origin.Y + (Transformed.Y + 0.5f) * Size.Y);
			CellPoints.emplace_back(static_cast<float>(X) + Transformed.X + 0.5f, static_cast<float>(Y) + Transformed.Y + 0.5f);
		}
		if (!Tile->bOneWay)
		{
			SolidPolygons.push_back(std::move(CellPoints));
		}
		if (TilemapMath::FlipsWinding(Flags))
		{
			std::reverse(Polygon.Points.begin(), Polygon.Points.end());
		}
		(Tile->bOneWay ? Shapes.OneWayPolygons : Shapes.Polygons).push_back(std::move(Polygon));
	});
	// 외곽선 (정적 물리 체인 — Full 칸 + 원웨이 아닌 다각형 타일 합집합)
	for (const std::vector<FVector2>& Loop : TraceSolidOutlines(SolidPolygons))
	{
		FTileCollisionOutline Outline;
		double                Area = 0.0;
		Outline.Points.reserve(Loop.size());
		for (size_t Index = 0; Index < Loop.size(); ++Index)
		{
			const FVector2& A = Loop[Index];
			const FVector2& B = Loop[(Index + 1) % Loop.size()];
			Area += static_cast<double>(A.X) * B.Y - static_cast<double>(B.X) * A.Y;
			Outline.Points.emplace_back(A.X * Size.X, A.Y * Size.Y);
		}
		Outline.bHole = Area < 0.0;
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

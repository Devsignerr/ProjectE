#include "Editor/Editor2D/Editor2DMath.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace
{
	// D4 원소 = 정규화 좌표 (u, v)에 곱하는 2x2 정수 행렬 { a, b, c, d } : u' = a·u + b·v, v' = c·u + d·v
	struct FMat2
	{
		int32 A = 1;
		int32 B = 0;
		int32 C = 0;
		int32 D = 1;

		bool operator==(const FMat2& Other) const = default;
	};

	FMat2 Multiply(const FMat2& L, const FMat2& R) // L ∘ R (R 먼저)
	{
		return { L.A * R.A + L.B * R.C, L.A * R.B + L.B * R.D, L.C * R.A + L.D * R.C, L.C * R.B + L.D * R.D };
	}

	// TilemapMath::TransformTileUv와 같은 순서: Rotate90 → FlipX → FlipY
	FMat2 MatrixFromFlags(uint32 Flags)
	{
		FMat2 Result;
		if ((Flags & TileCell::Rotate90Bit) != 0)
		{
			Result = Multiply(FMat2{ 0, -1, 1, 0 }, Result);
		}
		if ((Flags & TileCell::FlipXBit) != 0)
		{
			Result = Multiply(FMat2{ -1, 0, 0, 1 }, Result);
		}
		if ((Flags & TileCell::FlipYBit) != 0)
		{
			Result = Multiply(FMat2{ 1, 0, 0, -1 }, Result);
		}
		return Result;
	}

	uint32 FlagsFromMatrix(const FMat2& Matrix)
	{
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			const uint32 Flags = ((Index & 1) ? TileCell::Rotate90Bit : 0u) | ((Index & 2) ? TileCell::FlipXBit : 0u) | ((Index & 4) ? TileCell::FlipYBit : 0u);
			if (MatrixFromFlags(Flags) == Matrix)
			{
				return Flags;
			}
		}
		return 0; // 도달하지 않음 (8가지가 군 전체)
	}

	int32 PositiveModulo(int32 Value, int32 Divisor)
	{
		const int32 Result = Value % Divisor;
		return Result < 0 ? Result + Divisor : Result;
	}

	// 셀 하나 쓰기 (바뀌었으면 1)
	int32 WriteCell(FTilemapData& Data, int32 X, int32 Y, uint32 Cell)
	{
		if (Data.Get(X, Y) == Cell)
		{
			return 0;
		}
		Data.Set(X, Y, Cell);
		return 1;
	}
} // namespace

uint32 Editor2DMath::ComposeTileFlags(uint32 Outer, uint32 Inner)
{
	return FlagsFromMatrix(Multiply(MatrixFromFlags(Outer & TileCell::FlagMask), MatrixFromFlags(Inner & TileCell::FlagMask)));
}

uint32 Editor2DMath::TransformCell(uint32 Cell, uint32 Flags)
{
	if (TileCell::IsEmpty(Cell))
	{
		return Cell;
	}
	return (Cell & TileCell::IdMask) | ComposeTileFlags(Flags, TileCell::GetFlags(Cell));
}

Editor2DMath::FTileStamp Editor2DMath::FTileStamp::Single(uint32 Cell)
{
	FTileStamp Stamp;
	Stamp.Width  = 1;
	Stamp.Height = 1;
	Stamp.Cells  = { Cell };
	return Stamp;
}

Editor2DMath::FTileStamp Editor2DMath::TransformStamp(const FTileStamp& Stamp, uint32 Flags)
{
	if (!Stamp.IsValid())
	{
		return Stamp;
	}
	const FMat2 Matrix = MatrixFromFlags(Flags & TileCell::FlagMask);
	FTileStamp  Result;
	const bool  bSwap = (Flags & TileCell::Rotate90Bit) != 0;
	Result.Width      = bSwap ? Stamp.Height : Stamp.Width;
	Result.Height     = bSwap ? Stamp.Width : Stamp.Height;
	Result.Cells.assign(Stamp.Cells.size(), 0u);
	for (int32 Y = 0; Y < Stamp.Height; ++Y)
	{
		for (int32 X = 0; X < Stamp.Width; ++X)
		{
			// 가운데 기준 두 배 좌표 (반 칸도 정수)
			const int32 U  = 2 * X - (Stamp.Width - 1);
			const int32 V  = 2 * Y - (Stamp.Height - 1);
			const int32 NU = Matrix.A * U + Matrix.B * V;
			const int32 NV = Matrix.C * U + Matrix.D * V;
			const int32 NX = (NU + (Result.Width - 1)) / 2;
			const int32 NY = (NV + (Result.Height - 1)) / 2;
			Result.Set(NX, NY, TransformCell(Stamp.Get(X, Y), Flags));
		}
	}
	return Result;
}

Editor2DMath::FTileStamp Editor2DMath::MakeStampFromTileset(int32 Columns, int32 Col0, int32 Row0, int32 Col1, int32 Row1)
{
	FTileStamp Stamp;
	if (Columns <= 0)
	{
		return Stamp;
	}
	const int32 MinCol = std::max(0, std::min(Col0, Col1));
	const int32 MaxCol = std::min(Columns - 1, std::max(Col0, Col1));
	const int32 MinRow = std::max(0, std::min(Row0, Row1));
	const int32 MaxRow = std::max(MinRow, std::max(Row0, Row1));
	if (MinCol > MaxCol)
	{
		return Stamp;
	}
	Stamp.Width  = MaxCol - MinCol + 1;
	Stamp.Height = MaxRow - MinRow + 1;
	Stamp.Cells.assign(static_cast<size_t>(Stamp.Width) * static_cast<size_t>(Stamp.Height), 0u);
	for (int32 Y = 0; Y < Stamp.Height; ++Y)
	{
		const int32 Row = MaxRow - Y; // 스탬프 0행 = 이미지에서 가장 아래 행
		for (int32 X = 0; X < Stamp.Width; ++X)
		{
			Stamp.Set(X, Y, TileCell::Make(Row * Columns + MinCol + X));
		}
	}
	return Stamp;
}

FTileCoord Editor2DMath::GetStampOrigin(const FTileStamp& Stamp, const FTileCoord& Cursor)
{
	return { Cursor.X - (std::max(Stamp.Width, 1) - 1) / 2, Cursor.Y - (std::max(Stamp.Height, 1) - 1) / 2 };
}

uint32 Editor2DMath::SamplePattern(const FTileStamp& Stamp, int32 X, int32 Y, const FTileCoord& Origin)
{
	if (!Stamp.IsValid())
	{
		return 0;
	}
	return Stamp.Get(PositiveModulo(X - Origin.X, Stamp.Width), PositiveModulo(Y - Origin.Y, Stamp.Height));
}

int32 Editor2DMath::PaintStamp(FTilemapData& Data, const FTileStamp& Stamp, const FTileCoord& Cursor, bool bErase)
{
	if (!Stamp.IsValid())
	{
		return 0;
	}
	const FTileCoord Origin  = GetStampOrigin(Stamp, Cursor);
	int32            Changed = 0;
	for (int32 Y = 0; Y < Stamp.Height; ++Y)
	{
		for (int32 X = 0; X < Stamp.Width; ++X)
		{
			const uint32 Cell = Stamp.Get(X, Y);
			if (TileCell::IsEmpty(Cell))
			{
				continue; // 스탬프의 빈칸은 기존 셀을 건드리지 않는다
			}
			Changed += WriteCell(Data, Origin.X + X, Origin.Y + Y, bErase ? 0u : Cell);
		}
	}
	return Changed;
}

int32 Editor2DMath::FillRectPattern(FTilemapData& Data, const FTileRect& Rect, const FTileStamp& Stamp, bool bErase)
{
	if (!Rect.IsValid() || !Stamp.IsValid())
	{
		return 0;
	}
	const FTileCoord Origin{ Rect.MinX, Rect.MinY };
	int32            Changed = 0;
	for (int32 Y = Rect.MinY; Y <= Rect.MaxY; ++Y)
	{
		for (int32 X = Rect.MinX; X <= Rect.MaxX; ++X)
		{
			const uint32 Cell = SamplePattern(Stamp, X, Y, Origin);
			if (bErase)
			{
				Changed += WriteCell(Data, X, Y, 0u); // 지우개 사각형은 모양과 무관하게 전부
			}
			else if (!TileCell::IsEmpty(Cell))
			{
				Changed += WriteCell(Data, X, Y, Cell);
			}
		}
	}
	return Changed;
}

int32 Editor2DMath::LineStamp(FTilemapData& Data, const FTileCoord& From, const FTileCoord& To, const FTileStamp& Stamp, bool bErase)
{
	int32 Changed = 0;
	for (const FTileCoord& Cell : TilemapMath::LineCells(From.X, From.Y, To.X, To.Y))
	{
		Changed += PaintStamp(Data, Stamp, Cell, bErase);
	}
	return Changed;
}

FTileRect Editor2DMath::ComputeFloodLimit(const FTilemapData& Data, const FTileCoord& Start, int32 Margin)
{
	FTileRect Rect = Data.GetBounds();
	if (!Rect.IsValid())
	{
		Rect = { Start.X, Start.Y, Start.X, Start.Y };
	}
	Rect.MinX = std::min(Rect.MinX, Start.X) - Margin;
	Rect.MinY = std::min(Rect.MinY, Start.Y) - Margin;
	Rect.MaxX = std::max(Rect.MaxX, Start.X) + Margin;
	Rect.MaxY = std::max(Rect.MaxY, Start.Y) + Margin;
	return Rect;
}

bool Editor2DMath::CollectFloodRegion(const FTilemapData& Data, const FTileCoord& Start, const FTileRect& Limit, int32 MaxCells,
                                      std::vector<FTileCoord>& OutCells)
{
	OutCells.clear();
	if (!Limit.Contains(Start.X, Start.Y))
	{
		return true;
	}
	const uint32 Target = Data.Get(Start.X, Start.Y);
	const int32  Width  = Limit.GetWidth();
	std::vector<uint8> Visited(static_cast<size_t>(Width) * static_cast<size_t>(Limit.GetHeight()), 0);
	auto Index = [&](int32 X, int32 Y) { return static_cast<size_t>(Y - Limit.MinY) * static_cast<size_t>(Width) + static_cast<size_t>(X - Limit.MinX); };

	std::deque<FTileCoord> Queue;
	Queue.push_back(Start);
	Visited[Index(Start.X, Start.Y)] = 1;
	while (!Queue.empty())
	{
		const FTileCoord Cell = Queue.front();
		Queue.pop_front();
		OutCells.push_back(Cell);
		if (static_cast<int32>(OutCells.size()) > MaxCells)
		{
			OutCells.clear();
			return false;
		}
		const FTileCoord Neighbors[4] = { { Cell.X + 1, Cell.Y }, { Cell.X - 1, Cell.Y }, { Cell.X, Cell.Y + 1 }, { Cell.X, Cell.Y - 1 } };
		for (const FTileCoord& Next : Neighbors)
		{
			if (!Limit.Contains(Next.X, Next.Y) || Visited[Index(Next.X, Next.Y)] != 0 || Data.Get(Next.X, Next.Y) != Target)
			{
				continue;
			}
			Visited[Index(Next.X, Next.Y)] = 1;
			Queue.push_back(Next);
		}
	}
	return true;
}

int32 Editor2DMath::FloodFillPattern(FTilemapData& Data, const FTileCoord& Start, const FTileStamp& Stamp, bool bErase, const FTileRect& Limit, int32 MaxCells)
{
	if (!Stamp.IsValid())
	{
		return 0;
	}
	std::vector<FTileCoord> Region;
	if (!CollectFloodRegion(Data, Start, Limit, MaxCells, Region))
	{
		return -1;
	}
	const FTileCoord Origin  = GetStampOrigin(Stamp, Start);
	int32            Changed = 0;
	for (const FTileCoord& Cell : Region)
	{
		const uint32 Value = bErase ? 0u : SamplePattern(Stamp, Cell.X, Cell.Y, Origin);
		if (bErase || !TileCell::IsEmpty(Value))
		{
			Changed += WriteCell(Data, Cell.X, Cell.Y, Value);
		}
	}
	return Changed;
}

FVector2 Editor2DMath::ScreenToPlane(const FVector2& CameraPlane, float OrthoHeight, const FVector2& ImageSize, const FVector2& Pixel)
{
	if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return CameraPlane;
	}
	const float Width = OrthoHeight * ImageSize.X / ImageSize.Y;
	return FVector2(CameraPlane.X + (Pixel.X / ImageSize.X - 0.5f) * Width, CameraPlane.Y + (0.5f - Pixel.Y / ImageSize.Y) * OrthoHeight);
}

FVector2 Editor2DMath::PlaneToScreen(const FVector2& CameraPlane, float OrthoHeight, const FVector2& ImageSize, const FVector2& Plane)
{
	if (ImageSize.Y <= 0.0f || OrthoHeight <= 0.0f)
	{
		return FVector2::ZeroVector;
	}
	const float Width = OrthoHeight * ImageSize.X / ImageSize.Y;
	return FVector2(((Plane.X - CameraPlane.X) / Width + 0.5f) * ImageSize.X, (0.5f - (Plane.Y - CameraPlane.Y) / OrthoHeight) * ImageSize.Y);
}

FVector2 Editor2DMath::ZoomAroundCursor(const FVector2& CameraPlane, float OldHeight, float NewHeight, const FVector2& ImageSize, const FVector2& Pixel)
{
	const FVector2 Anchor = ScreenToPlane(CameraPlane, OldHeight, ImageSize, Pixel);
	// 같은 픽셀이 Anchor를 가리키도록: Anchor - (새 높이 기준 픽셀 오프셋)
	const FVector2 Offset = ScreenToPlane(FVector2::ZeroVector, NewHeight, ImageSize, Pixel);
	return Anchor - Offset;
}

FQuat Editor2DMath::Get2DCameraRotation()
{
	return FQuat::FromEuler(0.0f, -90.0f, 0.0f);
}

FQuat Editor2DMath::MakeRotation2D(float AngleDegrees)
{
	return FQuat::FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), -FMath::DegreesToRadians(AngleDegrees));
}

float Editor2DMath::SnapToStep(float Value, float Step, float Origin)
{
	if (Step <= 0.0f)
	{
		return Value;
	}
	return Origin + std::round((Value - Origin) / Step) * Step;
}

bool Editor2DMath::IsPointInConvexPolygon(const FVector2* Points, int32 Count, const FVector2& Point)
{
	if (Count < 3)
	{
		return false;
	}
	// 모든 변에 대해 같은 쪽 (감는 방향 무관, 경계 포함)
	bool bPositive = false;
	bool bNegative = false;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2& A     = Points[Index];
		const FVector2& B     = Points[(Index + 1) % Count];
		const float     Cross = FVector2::Cross(B - A, Point - A);
		bPositive             = bPositive || Cross > 0.0f;
		bNegative             = bNegative || Cross < 0.0f;
		if (bPositive && bNegative)
		{
			return false;
		}
	}
	return true;
}

bool Editor2DMath::IsPointInPolygon(const std::vector<FVector2>& Points, const FVector2& Point)
{
	bool         bInside = false;
	const size_t Count   = Points.size();
	if (Count < 3)
	{
		return false;
	}
	for (size_t Index = 0, Previous = Count - 1; Index < Count; Previous = Index++)
	{
		const FVector2& A = Points[Index];
		const FVector2& B = Points[Previous];
		if ((A.Y > Point.Y) != (B.Y > Point.Y) && Point.X < (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X)
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

float Editor2DMath::DistanceToSegment(const FVector2& A, const FVector2& B, const FVector2& Point)
{
	const FVector2 AB     = B - A;
	const float    Length = AB.LengthSquared();
	const float    T      = Length > 0.0f ? std::clamp(FVector2::Dot(Point - A, AB) / Length, 0.0f, 1.0f) : 0.0f;
	return (A + AB * T - Point).Length();
}

int32 Editor2DMath::FindFrontmost(const std::vector<FPick2DHit>& Hits)
{
	int32 Best = -1;
	for (int32 Index = 0; Index < static_cast<int32>(Hits.size()); ++Index)
	{
		if (Best < 0)
		{
			Best = Index;
			continue;
		}
		const FPick2DHit& A = Hits[static_cast<size_t>(Index)];
		const FPick2DHit& B = Hits[static_cast<size_t>(Best)];
		bool bBetter = false;
		if (A.Priority != B.Priority)
		{
			bBetter = A.Priority > B.Priority;
		}
		else if (A.SortLayer != B.SortLayer)
		{
			bBetter = A.SortLayer > B.SortLayer;
		}
		else if (A.Order != B.Order)
		{
			bBetter = A.Order > B.Order;
		}
		else
		{
			bBetter = A.Distance < B.Distance; // 같으면 먼저 넣은 것
		}
		if (bBetter)
		{
			Best = Index;
		}
	}
	return Best;
}

Editor2DMath::FScreenRect Editor2DMath::MakeScreenRect(const FVector2& A, const FVector2& B)
{
	FScreenRect Rect;
	Rect.Min = FVector2(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
	Rect.Max = FVector2(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y));
	return Rect;
}

bool Editor2DMath::ProjectBoundsToScreen(const FBox& Bounds, const FMatrix4x4& ViewProjection, const FVector2& ImageSize, FScreenRect& OutRect)
{
	if (!Bounds.IsValid())
	{
		return false;
	}
	bool bAny = false;
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector3 Point((Corner & 1) != 0 ? Bounds.Max.X : Bounds.Min.X, (Corner & 2) != 0 ? Bounds.Max.Y : Bounds.Min.Y,
		                     (Corner & 4) != 0 ? Bounds.Max.Z : Bounds.Min.Z);
		const FVector4 Clip = ViewProjection.TransformVector4(FVector4(Point, 1.0f));
		if (Clip.W <= 1.0e-4f)
		{
			continue; // 카메라 뒤
		}
		const FVector2 Pixel((Clip.X / Clip.W * 0.5f + 0.5f) * ImageSize.X, (0.5f - Clip.Y / Clip.W * 0.5f) * ImageSize.Y);
		UnionScreenRect(OutRect, bAny, FScreenRect{ Pixel, Pixel });
	}
	return bAny;
}

void Editor2DMath::UnionScreenRect(FScreenRect& InOut, bool& bInOutValid, const FScreenRect& Add)
{
	if (!bInOutValid)
	{
		InOut       = Add;
		bInOutValid = true;
		return;
	}
	InOut.Min = FVector2(FMath::Min(InOut.Min.X, Add.Min.X), FMath::Min(InOut.Min.Y, Add.Min.Y));
	InOut.Max = FVector2(FMath::Max(InOut.Max.X, Add.Max.X), FMath::Max(InOut.Max.Y, Add.Max.Y));
}

bool Editor2DMath::IsScreenRectInside(const FScreenRect& Inner, const FScreenRect& Outer)
{
	return Inner.Min.X >= Outer.Min.X && Inner.Min.Y >= Outer.Min.Y && Inner.Max.X <= Outer.Max.X && Inner.Max.Y <= Outer.Max.Y;
}

std::vector<FEntity> Editor2DMath::CombineBoxSelection(const std::vector<FEntity>& Current, const std::vector<FEntity>& Hits, EBoxSelectMode Mode)
{
	const auto Contains = [](const std::vector<FEntity>& List, FEntity Entity) { return std::find(List.begin(), List.end(), Entity) != List.end(); };
	std::vector<FEntity> UniqueHits;
	UniqueHits.reserve(Hits.size());
	for (const FEntity Entity : Hits)
	{
		if (Entity.IsValid() && !Contains(UniqueHits, Entity))
		{
			UniqueHits.push_back(Entity);
		}
	}
	if (Mode == EBoxSelectMode::Replace)
	{
		return UniqueHits;
	}
	std::vector<FEntity> Result;
	Result.reserve(Current.size() + UniqueHits.size());
	for (const FEntity Entity : Current)
	{
		if (Mode == EBoxSelectMode::Toggle && Contains(UniqueHits, Entity))
		{
			continue; // Ctrl: 이미 있던 것은 뺀다
		}
		Result.push_back(Entity);
	}
	for (const FEntity Entity : UniqueHits)
	{
		if (!Contains(Current, Entity))
		{
			Result.push_back(Entity);
		}
	}
	return Result;
}

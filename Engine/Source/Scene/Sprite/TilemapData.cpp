#include "Scene/Sprite/TilemapData.h"

#include "Scene/Terrain.h" // TerrainIO::EncodeBase64/DecodeBase64 (공용 base64)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <set>

namespace
{
	void WriteU8(std::vector<uint8>& Out, uint8 Value) { Out.push_back(Value); }

	void WriteU16(std::vector<uint8>& Out, uint16 Value)
	{
		Out.push_back(static_cast<uint8>(Value & 0xFFu));
		Out.push_back(static_cast<uint8>(Value >> 8));
	}

	void WriteU32(std::vector<uint8>& Out, uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
		{
			Out.push_back(static_cast<uint8>((Value >> Shift) & 0xFFu));
		}
	}

	// 잘림 검사 포함 리더
	struct FReader
	{
		const std::vector<uint8>& Bytes;
		size_t                    Offset = 0;

		bool ReadU8(uint8& Out)
		{
			if (Offset + 1 > Bytes.size())
			{
				return false;
			}
			Out = Bytes[Offset++];
			return true;
		}
		bool ReadU16(uint16& Out)
		{
			if (Offset + 2 > Bytes.size())
			{
				return false;
			}
			Out = static_cast<uint16>(Bytes[Offset] | (Bytes[Offset + 1] << 8));
			Offset += 2;
			return true;
		}
		bool ReadU32(uint32& Out)
		{
			if (Offset + 4 > Bytes.size())
			{
				return false;
			}
			Out = 0;
			for (int32 Byte = 0; Byte < 4; ++Byte)
			{
				Out |= static_cast<uint32>(Bytes[Offset + static_cast<size_t>(Byte)]) << (Byte * 8);
			}
			Offset += 4;
			return true;
		}
	};

	bool Fail(std::string* OutError, std::string Message)
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	}
} // namespace

FTileRect FTileRect::FromCorners(int32 X0, int32 Y0, int32 X1, int32 Y1)
{
	return { std::min(X0, X1), std::min(Y0, Y1), std::max(X0, X1), std::max(Y0, Y1) };
}

// ---- FTilemapData ----------------------------------------------------------------------------------------------------

uint32 FTilemapData::Get(int32 X, int32 Y) const
{
	const auto It = Chunks.find(MakeKey(X, Y));
	return It == Chunks.end() ? 0u : It->second.Cells[MakeLocalIndex(X, Y)];
}

void FTilemapData::Set(int32 X, int32 Y, uint32 Cell)
{
	if (TileCell::IsEmpty(Cell))
	{
		Cell = 0; // 플래그만 있는 값도 빈칸
	}
	const FChunkKey Key = MakeKey(X, Y);
	auto            It  = Chunks.find(Key);
	if (It == Chunks.end())
	{
		if (Cell == 0)
		{
			return;
		}
		It = Chunks.emplace(Key, FChunk{}).first;
	}
	FChunk&      Chunk = It->second;
	uint32&      Slot  = Chunk.Cells[MakeLocalIndex(X, Y)];
	const bool   bWas  = Slot != 0;
	const bool   bIs   = Cell != 0;
	Slot               = Cell;
	Chunk.Count += (bIs ? 1 : 0) - (bWas ? 1 : 0);
	if (Chunk.Count == 0)
	{
		Chunks.erase(It);
	}
}

size_t FTilemapData::GetCellCount() const
{
	size_t Count = 0;
	for (const auto& [Key, Chunk] : Chunks)
	{
		Count += static_cast<size_t>(Chunk.Count);
	}
	return Count;
}

FTileRect FTilemapData::GetBounds() const
{
	FTileRect Bounds;
	bool      bAny = false;
	ForEachCell([&](int32 X, int32 Y, uint32) {
		if (!bAny)
		{
			Bounds = { X, Y, X, Y };
			bAny   = true;
			return;
		}
		Bounds.MinX = std::min(Bounds.MinX, X);
		Bounds.MinY = std::min(Bounds.MinY, Y);
		Bounds.MaxX = std::max(Bounds.MaxX, X);
		Bounds.MaxY = std::max(Bounds.MaxY, Y);
	});
	return Bounds;
}

void FTilemapData::FillRect(const FTileRect& Rect, uint32 Cell)
{
	if (!Rect.IsValid())
	{
		return;
	}
	for (int32 Y = Rect.MinY; Y <= Rect.MaxY; ++Y)
	{
		for (int32 X = Rect.MinX; X <= Rect.MaxX; ++X)
		{
			Set(X, Y, Cell);
		}
	}
}

int32 FTilemapData::FloodFill(int32 X, int32 Y, uint32 Cell, const FTileRect& Limit, int32 MaxCells)
{
	if (TileCell::IsEmpty(Cell))
	{
		Cell = 0;
	}
	if (!Limit.Contains(X, Y))
	{
		return 0;
	}
	const uint32 Target = Get(X, Y);
	if (Target == Cell)
	{
		return 0;
	}
	// 먼저 영역을 모은다 (상한을 넘으면 바꾸지 않는다 — 반쯤 칠한 상태를 남기지 않게)
	std::set<std::pair<int32, int32>> Visited; // (Y, X)
	std::vector<FTileCoord>           Region;
	std::deque<FTileCoord>            Queue;
	Queue.push_back({ X, Y });
	Visited.insert({ Y, X });
	while (!Queue.empty())
	{
		const FTileCoord Current = Queue.front();
		Queue.pop_front();
		Region.push_back(Current);
		if (static_cast<int32>(Region.size()) > MaxCells)
		{
			return -1;
		}
		const FTileCoord Neighbors[4] = { { Current.X + 1, Current.Y }, { Current.X - 1, Current.Y }, { Current.X, Current.Y + 1 }, { Current.X, Current.Y - 1 } };
		for (const FTileCoord& Next : Neighbors)
		{
			if (Limit.Contains(Next.X, Next.Y) && Get(Next.X, Next.Y) == Target && Visited.insert({ Next.Y, Next.X }).second)
			{
				Queue.push_back(Next);
			}
		}
	}
	for (const FTileCoord& Coord : Region)
	{
		Set(Coord.X, Coord.Y, Cell);
	}
	return static_cast<int32>(Region.size());
}

int32 FTilemapData::DrawLine(int32 X0, int32 Y0, int32 X1, int32 Y1, uint32 Cell)
{
	const std::vector<FTileCoord> Cells = TilemapMath::LineCells(X0, Y0, X1, Y1);
	for (const FTileCoord& Coord : Cells)
	{
		Set(Coord.X, Coord.Y, Cell);
	}
	return static_cast<int32>(Cells.size());
}

FTilemapRegion FTilemapData::CopyRegion(const FTileRect& Rect) const
{
	FTilemapRegion Region;
	if (!Rect.IsValid())
	{
		return Region;
	}
	Region.Width  = Rect.GetWidth();
	Region.Height = Rect.GetHeight();
	Region.Cells.resize(static_cast<size_t>(Region.Width) * static_cast<size_t>(Region.Height));
	for (int32 Y = 0; Y < Region.Height; ++Y)
	{
		for (int32 X = 0; X < Region.Width; ++X)
		{
			Region.Cells[static_cast<size_t>(Y) * static_cast<size_t>(Region.Width) + static_cast<size_t>(X)] = Get(Rect.MinX + X, Rect.MinY + Y);
		}
	}
	return Region;
}

void FTilemapData::PasteRegion(const FTilemapRegion& Region, int32 X, int32 Y, bool bSkipEmpty)
{
	if (Region.Width <= 0 || Region.Height <= 0 || Region.Cells.size() != static_cast<size_t>(Region.Width) * static_cast<size_t>(Region.Height))
	{
		return;
	}
	for (int32 RegionY = 0; RegionY < Region.Height; ++RegionY)
	{
		for (int32 RegionX = 0; RegionX < Region.Width; ++RegionX)
		{
			const uint32 Cell = Region.Get(RegionX, RegionY);
			if (Cell == 0 && bSkipEmpty)
			{
				continue;
			}
			Set(X + RegionX, Y + RegionY, Cell);
		}
	}
}

std::string FTilemapData::Encode() const
{
	if (Chunks.empty())
	{
		return {};
	}
	std::vector<uint8> Bytes;
	Bytes.reserve(5 + Chunks.size() * 32);
	WriteU8(Bytes, EncodingVersion);
	WriteU32(Bytes, static_cast<uint32>(Chunks.size()));
	for (const auto& [Key, Chunk] : Chunks) // std::map: (Y, X) 오름차순 = 결정적
	{
		WriteU32(Bytes, static_cast<uint32>(Key.second));
		WriteU32(Bytes, static_cast<uint32>(Key.first));
		int32 Index = 0;
		while (Index < ChunkCellCount)
		{
			const uint32 Value = Chunk.Cells[static_cast<size_t>(Index)];
			int32        Run   = 1;
			while (Index + Run < ChunkCellCount && Chunk.Cells[static_cast<size_t>(Index + Run)] == Value)
			{
				++Run;
			}
			WriteU16(Bytes, static_cast<uint16>(Run));
			WriteU32(Bytes, Value);
			Index += Run;
		}
	}
	return TerrainIO::EncodeBase64(Bytes.data(), Bytes.size());
}

bool FTilemapData::Decode(std::string_view Text, FTilemapData& OutData, std::string* OutError)
{
	OutData.Clear();
	if (Text.empty())
	{
		return true;
	}
	std::vector<uint8> Bytes;
	if (!TerrainIO::DecodeBase64(Text, Bytes))
	{
		return Fail(OutError, "타일 데이터 base64 오류");
	}
	FReader Reader{ Bytes };
	uint8   Version    = 0;
	uint32  ChunkCount = 0;
	if (!Reader.ReadU8(Version) || !Reader.ReadU32(ChunkCount))
	{
		return Fail(OutError, "타일 데이터가 잘렸습니다 (머리)");
	}
	if (Version != EncodingVersion)
	{
		return Fail(OutError, std::format("타일 데이터 형식 버전 {}을 모릅니다 (현재 {})", Version, EncodingVersion));
	}
	// 청크 하나는 최소 8 + 6바이트 — 개수가 터무니없으면 할당 전에 거른다
	if (static_cast<uint64>(ChunkCount) * 14u > Bytes.size())
	{
		return Fail(OutError, std::format("타일 데이터 청크 수 {}가 데이터 크기와 맞지 않습니다", ChunkCount));
	}
	FTilemapData Result;
	for (uint32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		uint32 RawX = 0;
		uint32 RawY = 0;
		if (!Reader.ReadU32(RawX) || !Reader.ReadU32(RawY))
		{
			return Fail(OutError, "타일 데이터가 잘렸습니다 (청크 좌표)");
		}
		const int32 ChunkX = static_cast<int32>(RawX);
		const int32 ChunkY = static_cast<int32>(RawY);
		if (std::abs(static_cast<int64>(ChunkX)) > MaxChunkCoord || std::abs(static_cast<int64>(ChunkY)) > MaxChunkCoord)
		{
			return Fail(OutError, std::format("타일 데이터 청크 좌표 ({}, {})가 범위 밖입니다", ChunkX, ChunkY));
		}
		const FChunkKey Key{ ChunkY, ChunkX };
		if (Result.Chunks.contains(Key))
		{
			return Fail(OutError, std::format("타일 데이터 청크 ({}, {})가 중복됩니다", ChunkX, ChunkY));
		}
		FChunk Chunk;
		int32  Index = 0;
		while (Index < ChunkCellCount)
		{
			uint16 Run   = 0;
			uint32 Value = 0;
			if (!Reader.ReadU16(Run) || !Reader.ReadU32(Value))
			{
				return Fail(OutError, "타일 데이터가 잘렸습니다 (RLE)");
			}
			if (Run == 0 || Index + Run > ChunkCellCount)
			{
				return Fail(OutError, std::format("타일 데이터 RLE 길이 {}가 잘못되었습니다", Run));
			}
			if (TileCell::IsEmpty(Value))
			{
				Value = 0;
			}
			std::fill_n(Chunk.Cells.begin() + Index, Run, Value);
			if (Value != 0)
			{
				Chunk.Count += Run;
			}
			Index += Run;
		}
		if (Chunk.Count > 0)
		{
			Result.Chunks.emplace(Key, Chunk);
		}
	}
	if (Reader.Offset != Bytes.size())
	{
		return Fail(OutError, std::format("타일 데이터 끝에 남는 바이트 {}개", Bytes.size() - Reader.Offset));
	}
	OutData = std::move(Result);
	return true;
}

bool FTilemapData::operator==(const FTilemapData& Other) const
{
	if (Chunks.size() != Other.Chunks.size())
	{
		return false;
	}
	for (auto It = Chunks.begin(), OtherIt = Other.Chunks.begin(); It != Chunks.end(); ++It, ++OtherIt)
	{
		if (It->first != OtherIt->first || It->second.Cells != OtherIt->second.Cells)
		{
			return false;
		}
	}
	return true;
}

// ---- TilemapMath -----------------------------------------------------------------------------------------------------

FVector2 TilemapMath::CellToLocal(int32 X, int32 Y, const FVector2& CellSize)
{
	return FVector2(static_cast<float>(X) * CellSize.X, static_cast<float>(Y) * CellSize.Y);
}

FVector2 TilemapMath::CellCenterToLocal(int32 X, int32 Y, const FVector2& CellSize)
{
	return FVector2((static_cast<float>(X) + 0.5f) * CellSize.X, (static_cast<float>(Y) + 0.5f) * CellSize.Y);
}

FTileCoord TilemapMath::LocalToCell(const FVector2& Local, const FVector2& CellSize)
{
	if (!(CellSize.X > 0.0f) || !(CellSize.Y > 0.0f))
	{
		return {};
	}
	return { static_cast<int32>(std::floor(Local.X / CellSize.X)), static_cast<int32>(std::floor(Local.Y / CellSize.Y)) };
}

std::vector<FTileCoord> TilemapMath::LineCells(int32 X0, int32 Y0, int32 X1, int32 Y1)
{
	std::vector<FTileCoord> Cells;
	const int64 Dx = std::abs(static_cast<int64>(X1) - X0);
	const int64 Dy = -std::abs(static_cast<int64>(Y1) - Y0);
	const int32 Sx = X0 < X1 ? 1 : -1;
	const int32 Sy = Y0 < Y1 ? 1 : -1;
	Cells.reserve(static_cast<size_t>(std::max(Dx, -Dy) + 1));
	int64 Error = Dx + Dy;
	int32 X     = X0;
	int32 Y     = Y0;
	while (true)
	{
		Cells.push_back({ X, Y });
		if (X == X1 && Y == Y1)
		{
			break;
		}
		const int64 Twice = 2 * Error;
		if (Twice >= Dy)
		{
			Error += Dy;
			X += Sx;
		}
		if (Twice <= Dx)
		{
			Error += Dx;
			Y += Sy;
		}
	}
	return Cells;
}

FVector2 TilemapMath::TransformTileUv(const FVector2& Normalized, uint32 Flags)
{
	FVector2 Result = Normalized;
	if ((Flags & TileCell::Rotate90Bit) != 0)
	{
		Result = FVector2(-Result.Y, Result.X);
	}
	if ((Flags & TileCell::FlipXBit) != 0)
	{
		Result.X = -Result.X;
	}
	if ((Flags & TileCell::FlipYBit) != 0)
	{
		Result.Y = -Result.Y;
	}
	return Result;
}

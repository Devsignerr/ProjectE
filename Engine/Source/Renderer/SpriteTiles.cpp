#include "Renderer/SpriteTiles.h"

#include "Scene/Sprite/TilemapData.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <cmath>

SpriteTiles::FTileQuad SpriteTiles::ComputeTileQuad(int32 X, int32 Y, uint32 CellFlags, const FVector2& CellSize)
{
	const FVector2 Center = TilemapMath::CellCenterToLocal(X, Y, CellSize);
	const FVector2 Corner = TilemapMath::TransformTileUv(FVector2(-0.5f, -0.5f), CellFlags);
	const FVector2 AxisU  = TilemapMath::TransformTileUv(FVector2(1.0f, 0.0f), CellFlags);
	const FVector2 AxisV  = TilemapMath::TransformTileUv(FVector2(0.0f, 1.0f), CellFlags);
	FTileQuad      Quad;
	Quad.Origin = FVector3(Center.X + Corner.X * CellSize.X, 0.0f, Center.Y + Corner.Y * CellSize.Y);
	Quad.AxisX  = FVector3(AxisU.X * CellSize.X, 0.0f, AxisU.Y * CellSize.Y);
	Quad.AxisZ  = FVector3(AxisV.X * CellSize.X, 0.0f, AxisV.Y * CellSize.Y);
	return Quad;
}

FSpriteInstanceGpu SpriteTiles::MakeTileInstance(int32 X, int32 Y, uint32 CellFlags, int32 TileId, const FTilesetAsset& Tileset, const FVector2& CellSize)
{
	const FTileQuad     Quad = ComputeTileQuad(X, Y, CellFlags, CellSize);
	const FSpriteUvRect Uv   = Tileset.ComputeTileUv(TileId);
	FSpriteInstanceGpu  Instance;
	Instance.Origin       = Quad.Origin;
	Instance.TextureIndex = 0;
	Instance.AxisX        = Quad.AxisX;
	Instance.Flags        = Tileset.Filter == ESpriteFilter::Point ? FlagPoint : 0u;
	Instance.AxisZ        = Quad.AxisZ;
	Instance.AlphaCutoff  = 0.5f;
	Instance.UVRect       = FVector4(Uv.U0, Uv.V0, Uv.U1, Uv.V1);
	Instance.Color        = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	return Instance;
}

int32 SpriteTiles::SelectAnimationFrame(const FTileDefinition& Definition, int32 BaseTileId, double Time)
{
	double Total = 0.0;
	for (const FTileAnimFrame& Frame : Definition.Animation)
	{
		Total += Frame.Duration > 0.0f ? static_cast<double>(Frame.Duration) : 0.0;
	}
	if (Total <= 0.0)
	{
		return BaseTileId;
	}
	double Local = std::fmod(Time, Total);
	if (Local < 0.0)
	{
		Local += Total;
	}
	double Start = 0.0;
	for (const FTileAnimFrame& Frame : Definition.Animation)
	{
		if (Frame.Duration <= 0.0f)
		{
			continue;
		}
		Start += static_cast<double>(Frame.Duration);
		if (Local < Start)
		{
			return Frame.TileId;
		}
	}
	// 부동소수 경계 (Local이 Total에 아주 가까움): 마지막 유효 프레임
	for (auto It = Definition.Animation.rbegin(); It != Definition.Animation.rend(); ++It)
	{
		if (It->Duration > 0.0f)
		{
			return It->TileId;
		}
	}
	return BaseTileId;
}

uint64 SpriteTiles::HashCells(std::span<const FCellRecord> Cells)
{
	uint64     Hash  = 14695981039346656037ull;
	const auto Mix   = [&Hash](uint32 Value) {
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Hash ^= (Value >> (Byte * 8)) & 0xFFu;
			Hash *= 1099511628211ull;
		}
	};
	Mix(static_cast<uint32>(Cells.size()));
	for (const FCellRecord& Record : Cells)
	{
		Mix(static_cast<uint32>(Record.X));
		Mix(static_cast<uint32>(Record.Y));
		Mix(Record.Cell);
	}
	return Hash;
}

void SpriteTiles::BuildChunk(std::span<const FCellRecord> Cells, const FTilesetAsset& Tileset, const FVector2& CellSize, FChunkBuild& Out)
{
	Out.Instances.clear();
	Out.Animated.clear();
	Out.LocalBounds = FBox();
	Out.Hash        = HashCells(Cells);
	Out.Instances.reserve(Cells.size());
	for (const FCellRecord& Record : Cells)
	{
		const int32 TileId = TileCell::GetTileId(Record.Cell);
		if (TileId < 0)
		{
			continue;
		}
		const FVector2 Corner = TilemapMath::CellToLocal(Record.X, Record.Y, CellSize);
		Out.LocalBounds.AddPoint(FVector3(Corner.X, 0.0f, Corner.Y));
		Out.LocalBounds.AddPoint(FVector3(Corner.X + CellSize.X, 0.0f, Corner.Y + CellSize.Y));
		const FTileDefinition* Definition = Tileset.FindTile(TileId);
		if (Definition != nullptr && !Definition->Animation.empty())
		{
			Out.Animated.push_back({ Record.X, Record.Y, Record.Cell });
			continue;
		}
		Out.Instances.push_back(MakeTileInstance(Record.X, Record.Y, TileCell::GetFlags(Record.Cell), TileId, Tileset, CellSize));
	}
}

#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 타일맵 셀 데이터 (Phase 56 — 2D 규약은 Scene/Sprite/SpriteAsset.h 머리 주석).
//
// 셀 좌표 (X, Y): 정수, 음수 허용. +X = 엔티티 로컬 +X(오른쪽), +Y = 로컬 +Z(위). 셀 (0,0)의 왼쪽 아래 모서리 = 엔티티 원점.
//   셀 (x, y)가 덮는 로컬 사각형 = X [x·폭, (x+1)·폭], Z [y·높이, (y+1)·높이] (폭/높이 = 셀 크기 cm, TilemapMath).
//
// 셀 값 uint32 (TileCell): 0 = 빈칸. 비트 0~28 = 타일 Id + 1 (Id 0 ~ 2^29-2), 비트 29 = FlipX, 30 = FlipY, 31 = Rotate90.
//   타일 변환은 타일 가운데 기준 정규화 좌표(u 오른쪽, v 위, -0.5~0.5)에서 Rotate90(반시계 90°: (u, v) → (-v, u)) → FlipX(u → -u)
//   → FlipY(v → -v) 순서로 적용한다 (렌더러 UV·충돌 다각형 공통. 정사각형이 아닌 타일의 회전은 칸 안으로 늘여 맞춘다).
//
// 저장: 32x32 청크 희소 맵 (빈 청크는 지운다). 청크 좌표 = 셀 좌표 >> 5 (음수는 내림), 청크 안 = 셀 & 31.
//
// 인코딩 (FTilemapComponent::TileData 문자열, Encode/Decode): base64(이진). 빈 맵 = 빈 문자열.
//   이진(리틀 엔디언): uint8 형식 버전(EncodingVersion) | uint32 청크 수 | 청크마다 { int32 청크 X | int32 청크 Y | RLE }.
//   청크는 (Y, X) 오름차순 — 같은 데이터는 항상 같은 문자열(결정적, 씬 diff 안정). RLE = 청크 안 1024칸을 (y, x) 행 우선으로
//   { uint16 길이(1~1024) | uint32 값 } 반복, 길이 합은 정확히 1024. 빈 청크는 쓰지 않는다.
//   손상 입력(base64 오류, 잘림, 길이 합 불일치, 중복 청크, 버전 불일치, 범위 밖 청크 좌표)은 false + 오류 문구, 결과는 빈 맵.
namespace TileCell
{
	constexpr uint32 IdMask      = 0x1FFFFFFFu;
	constexpr uint32 FlipXBit    = 1u << 29;
	constexpr uint32 FlipYBit    = 1u << 30;
	constexpr uint32 Rotate90Bit = 1u << 31;
	constexpr uint32 FlagMask    = FlipXBit | FlipYBit | Rotate90Bit;
	constexpr int32  MaxTileId   = static_cast<int32>(IdMask) - 1;

	// TileId < 0 이거나 너무 크면 0(빈칸)
	constexpr uint32 Make(int32 TileId, uint32 Flags = 0)
	{
		return (TileId < 0 || TileId > MaxTileId) ? 0u : ((static_cast<uint32>(TileId) + 1u) | (Flags & FlagMask));
	}
	constexpr bool   IsEmpty(uint32 Cell) { return (Cell & IdMask) == 0; }
	constexpr int32  GetTileId(uint32 Cell) { return IsEmpty(Cell) ? -1 : static_cast<int32>((Cell & IdMask) - 1u); } // 빈칸 -1
	constexpr uint32 GetFlags(uint32 Cell) { return Cell & FlagMask; }
} // namespace TileCell

struct FTileCoord
{
	int32 X = 0;
	int32 Y = 0;

	bool operator==(const FTileCoord& Other) const = default;
};

// 셀 사각형 (양 끝 포함)
struct FTileRect
{
	int32 MinX = 0;
	int32 MinY = 0;
	int32 MaxX = -1;
	int32 MaxY = -1;

	bool  IsValid() const { return MinX <= MaxX && MinY <= MaxY; }
	bool  Contains(int32 X, int32 Y) const { return X >= MinX && X <= MaxX && Y >= MinY && Y <= MaxY; }
	int32 GetWidth() const { return IsValid() ? MaxX - MinX + 1 : 0; }
	int32 GetHeight() const { return IsValid() ? MaxY - MinY + 1 : 0; }
	static FTileRect FromCorners(int32 X0, int32 Y0, int32 X1, int32 Y1); // 순서 무관
	bool operator==(const FTileRect& Other) const = default;
};

// 박스 선택 복사/붙이기 영역 (Width x Height, 행 우선 (y, x) — 0번 = 영역 왼쪽 아래)
struct FTilemapRegion
{
	int32               Width  = 0;
	int32               Height = 0;
	std::vector<uint32> Cells;

	uint32 Get(int32 X, int32 Y) const { return Cells[static_cast<size_t>(Y) * static_cast<size_t>(Width) + static_cast<size_t>(X)]; }
	bool operator==(const FTilemapRegion& Other) const = default;
};

class FTilemapData
{
public:
	static constexpr int32  ChunkSize       = 32;
	static constexpr int32  ChunkShift      = 5;
	static constexpr int32  ChunkCellCount  = ChunkSize * ChunkSize;
	static constexpr uint8  EncodingVersion = 1;
	static constexpr int32  MaxChunkCoord   = (1 << 25) - 1; // |청크 좌표| 상한 (셀 좌표 int32 안)
	static constexpr int32  DefaultFloodFillLimit = 65536;

	uint32 Get(int32 X, int32 Y) const;
	void   Set(int32 X, int32 Y, uint32 Cell); // Cell 0 = 지우기
	void   Erase(int32 X, int32 Y) { Set(X, Y, 0); }
	void   Clear() { Chunks.clear(); }

	bool   IsEmpty() const { return Chunks.empty(); }
	size_t GetCellCount() const; // 빈칸이 아닌 셀 수
	size_t GetChunkCount() const { return Chunks.size(); }
	// 빈칸이 아닌 셀의 경계 (빈 맵이면 무효 사각형)
	FTileRect GetBounds() const;

	// ---- 편집 연산 (Cell 0 = 지우기)
	void FillRect(const FTileRect& Rect, uint32 Cell);
	// 시작 셀과 같은 값인 4방향 연결 영역을 Cell로 바꾼다. Limit 밖은 넘지 않는다(빈칸 영역은 무한하므로 경계가 필요하다).
	// 영역이 MaxCells를 넘으면 아무것도 바꾸지 않고 -1. 시작 셀이 Limit 밖이거나 이미 Cell이면 0. 반환 = 바꾼 셀 수
	int32 FloodFill(int32 X, int32 Y, uint32 Cell, const FTileRect& Limit, int32 MaxCells = DefaultFloodFillLimit);
	// 브레젠험 선 (양 끝 포함). 반환 = 칠한 셀 수
	int32 DrawLine(int32 X0, int32 Y0, int32 X1, int32 Y1, uint32 Cell);
	FTilemapRegion CopyRegion(const FTileRect& Rect) const;
	// 영역 왼쪽 아래를 (X, Y)에 놓는다. bSkipEmpty면 영역의 빈칸은 기존 셀을 지우지 않는다
	void PasteRegion(const FTilemapRegion& Region, int32 X, int32 Y, bool bSkipEmpty = true);

	// 빈칸이 아닌 셀을 청크 (Y, X) 오름차순 → 청크 안 (y, x) 행 우선으로 방문: Visitor(int32 X, int32 Y, uint32 Cell)
	template <typename TVisitor>
	void ForEachCell(TVisitor&& Visitor) const
	{
		for (const auto& [Key, Chunk] : Chunks)
		{
			const int32 BaseX = Key.second * ChunkSize;
			const int32 BaseY = Key.first * ChunkSize;
			for (int32 Index = 0; Index < ChunkCellCount; ++Index)
			{
				if (const uint32 Cell = Chunk.Cells[static_cast<size_t>(Index)]; Cell != 0)
				{
					Visitor(BaseX + (Index & (ChunkSize - 1)), BaseY + (Index >> ChunkShift), Cell);
				}
			}
		}
	}

	std::string Encode() const;
	// 실패하면 OutData는 빈 맵 + false (OutError에 이유)
	static bool Decode(std::string_view Text, FTilemapData& OutData, std::string* OutError = nullptr);

	bool operator==(const FTilemapData& Other) const;

private:
	struct FChunk
	{
		std::array<uint32, ChunkCellCount> Cells{};
		int32                              Count = 0; // 빈칸이 아닌 셀 수
	};
	using FChunkKey = std::pair<int32, int32>; // (청크 Y, 청크 X) — 정렬 순서 = 인코딩 순서

	static FChunkKey MakeKey(int32 X, int32 Y) { return { Y >> ChunkShift, X >> ChunkShift }; }
	static size_t    MakeLocalIndex(int32 X, int32 Y) { return static_cast<size_t>(((Y & (ChunkSize - 1)) << ChunkShift) | (X & (ChunkSize - 1))); }

	std::map<FChunkKey, FChunk> Chunks;
};

// 셀 ↔ 엔티티 로컬 좌표 (FVector2 = (로컬 X, 로컬 Z) cm)
namespace TilemapMath
{
	FVector2   CellToLocal(int32 X, int32 Y, const FVector2& CellSize);       // 셀 왼쪽 아래 모서리
	FVector2   CellCenterToLocal(int32 X, int32 Y, const FVector2& CellSize); // 셀 가운데
	FTileCoord LocalToCell(const FVector2& Local, const FVector2& CellSize);  // 내림 (경계는 오른쪽/위 셀)
	// 브레젠험 선 셀 목록 (양 끝 포함, 시작 → 끝 순서)
	std::vector<FTileCoord> LineCells(int32 X0, int32 Y0, int32 X1, int32 Y1);
	// 타일 정규화 좌표(u, v ∈ [-0.5, 0.5], v 위) 변환 — 셀 플래그(Rotate90 → FlipX → FlipY)
	FVector2 TransformTileUv(const FVector2& Normalized, uint32 Flags);
	// 플래그가 와인딩을 뒤집는가 (FlipX xor FlipY — 회전은 와인딩 유지)
	constexpr bool FlipsWinding(uint32 Flags) { return ((Flags & TileCell::FlipXBit) != 0) != ((Flags & TileCell::FlipYBit) != 0); }
} // namespace TilemapMath

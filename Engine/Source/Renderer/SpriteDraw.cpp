#include "Renderer/SpriteDraw.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace SpriteMath
{
	FQuad ComputeQuad(const FSpriteDrawItem& Item)
	{
		const FVector3 LocalOrigin(-Item.Pivot.X * Item.Size.X, 0.0f, -Item.Pivot.Y * Item.Size.Y);
		FQuad          Quad;
		Quad.Origin = Item.World.TransformPosition(LocalOrigin);
		Quad.AxisX  = Item.World.TransformVector(FVector3(Item.Size.X, 0.0f, 0.0f));
		Quad.AxisZ  = Item.World.TransformVector(FVector3(0.0f, 0.0f, Item.Size.Y));
		return Quad;
	}

	FVector3 GetCornerPosition(const FQuad& Quad, float U, float V)
	{
		return Quad.Origin + Quad.AxisX * U + Quad.AxisZ * V;
	}

	FVector2 GetCornerUV(const FSpriteDrawItem& Item, float U, float V)
	{
		const float T = 1.0f - V;
		return FVector2(Item.UVMin.X + (Item.UVMax.X - Item.UVMin.X) * U, Item.UVMin.Y + (Item.UVMax.Y - Item.UVMin.Y) * T);
	}

	float ComputeSortDepth(const FQuad& Quad, const FVector3& CameraPosition, const FVector3& CameraForward)
	{
		const FVector3 Center = Quad.Origin + (Quad.AxisX + Quad.AxisZ) * 0.5f;
		return FVector3::Dot(Center - CameraPosition, CameraForward);
	}
} // namespace SpriteMath

namespace SpriteNineSlice
{
	namespace
	{
		constexpr float MinPieceSize = 1e-4f;

		// 한 축 조각: 위치 [Begin, End] (cm, 피벗 원점) + 텍스처 비율 [TexBegin, TexEnd] (0 = 낮은 쪽 끝 — X는 왼쪽, Z는 아래)
		struct FSegment
		{
			float Begin    = 0.0f;
			float End      = 0.0f;
			float TexBegin = 0.0f;
			float TexEnd   = 0.0f;
		};

		void BuildAxis(float Size, float Original, float BorderLow, float BorderHigh, float Pivot, ESpriteSliceMode Mode, std::vector<FSegment>& Out)
		{
			Out.clear();
			const float Low = -Pivot * Size;
			if (Original <= MinPieceSize || Size <= MinPieceSize)
			{
				if (Size > MinPieceSize)
				{
					Out.push_back({ Low, Low + Size, 0.0f, 1.0f });
				}
				return;
			}
			// 텍스처 비율은 원래 테두리 기준 (줄여 그려도 테두리 텍스처 전체를 압축해 보인다)
			const float TexLow  = FMath::Clamp(BorderLow / Original, 0.0f, 1.0f);
			const float TexHigh = FMath::Max(TexLow, FMath::Clamp(1.0f - BorderHigh / Original, 0.0f, 1.0f));
			float       Lo      = BorderLow;
			float       Hi      = BorderHigh;
			if (Lo + Hi > Size)
			{
				const float Scale = Size / (Lo + Hi); // 테두리 합보다 작으면 비율로 줄임 (가운데 폭 0)
				Lo *= Scale;
				Hi *= Scale;
			}
			const auto Push = [&Out](float Begin, float End, float TexBegin, float TexEnd) {
				if (End - Begin > MinPieceSize)
				{
					Out.push_back({ Begin, End, TexBegin, TexEnd });
				}
			};
			Push(Low, Low + Lo, 0.0f, TexLow);
			const float MiddleBegin = Low + Lo;
			const float MiddleEnd   = Low + Size - Hi;
			const float TileLength  = Original - BorderLow - BorderHigh;
			const float Middle      = MiddleEnd - MiddleBegin;
			bool        bTiled      = false;
			if (Mode == ESpriteSliceMode::Tile && TileLength > MinPieceSize && Middle > MinPieceSize)
			{
				const int32 Count = static_cast<int32>(std::ceil(Middle / TileLength - 1e-4f));
				if (Count <= MaxTilesPerAxis)
				{
					for (int32 Index = 0; Index < Count; ++Index)
					{
						const float Begin = MiddleBegin + static_cast<float>(Index) * TileLength;
						const float End   = FMath::Min(Begin + TileLength, MiddleEnd);
						Push(Begin, End, TexLow, TexLow + (TexHigh - TexLow) * ((End - Begin) / TileLength));
					}
					bTiled = true;
				}
			}
			if (!bTiled)
			{
				Push(MiddleBegin, MiddleEnd, TexLow, TexHigh);
			}
			Push(Low + Size - Hi, Low + Size, TexHigh, 1.0f);
		}
	} // namespace

	bool ShouldSlice(const FSpriteSlice& Slice, const FVector2& Size, const FVector2& OriginalSize)
	{
		constexpr float Tolerance = 1e-3f;
		return Slice.HasBorder() && (std::abs(Size.X - OriginalSize.X) > Tolerance || std::abs(Size.Y - OriginalSize.Y) > Tolerance);
	}

	void Build(const FInput& Input, std::vector<FPiece>& Out)
	{
		Out.clear();
		thread_local std::vector<FSegment> Columns;
		thread_local std::vector<FSegment> Rows;
		BuildAxis(Input.Size.X, Input.OriginalSize.X, Input.BorderLeft, Input.BorderRight, Input.Pivot.X, Input.Mode, Columns);
		BuildAxis(Input.Size.Y, Input.OriginalSize.Y, Input.BorderBottom, Input.BorderTop, Input.Pivot.Y, Input.Mode, Rows);
		Out.reserve(Columns.size() * Rows.size());
		for (const FSegment& Row : Rows)
		{
			for (const FSegment& Column : Columns)
			{
				FPiece Piece;
				Piece.Min = FVector2(Column.Begin, Row.Begin);
				Piece.Max = FVector2(Column.End, Row.End);
				// U는 왼쪽 → 오른쪽, V는 아래 = UVMax.Y → 위 = UVMin.Y
				Piece.UVMin = FVector2(Input.UVMin.X + (Input.UVMax.X - Input.UVMin.X) * Column.TexBegin,
				                       Input.UVMax.Y + (Input.UVMin.Y - Input.UVMax.Y) * Row.TexEnd);
				Piece.UVMax = FVector2(Input.UVMin.X + (Input.UVMax.X - Input.UVMin.X) * Column.TexEnd,
				                       Input.UVMax.Y + (Input.UVMin.Y - Input.UVMax.Y) * Row.TexBegin);
				// 반전: 피벗(원점)을 지나는 축 거울 — 위치 부호 반전 + UV 교환
				if (Input.bFlipX)
				{
					Piece.Min.X = -Column.End;
					Piece.Max.X = -Column.Begin;
					std::swap(Piece.UVMin.X, Piece.UVMax.X);
				}
				if (Input.bFlipY)
				{
					Piece.Min.Y = -Row.End;
					Piece.Max.Y = -Row.Begin;
					std::swap(Piece.UVMin.Y, Piece.UVMax.Y);
				}
				Out.push_back(Piece);
			}
		}
	}

	FSpriteDrawItem MakePieceItem(const FSpriteDrawItem& Base, const FPiece& Piece)
	{
		// 로컬 사각형 [-Pivot, 1 - Pivot] × Size = [Min, Max] → Size = Max - Min, Pivot = -Min / Size (크기 0 조각은 Build가 만들지 않음)
		FSpriteDrawItem Item = Base;
		Item.Size            = Piece.Max - Piece.Min;
		Item.Pivot           = FVector2(-Piece.Min.X / Item.Size.X, -Piece.Min.Y / Item.Size.Y);
		Item.UVMin           = Piece.UVMin;
		Item.UVMax           = Piece.UVMax;
		return Item;
	}
} // namespace SpriteNineSlice

namespace SpriteSorting
{
	uint32 DepthToDescendingBits(float Depth)
	{
		if (!(Depth == Depth) || Depth == 0.0f)
		{
			Depth = 0.0f; // NaN·-0 정리 (결정적 순서)
		}
		const uint32 Bits      = std::bit_cast<uint32>(Depth);
		const uint32 Ascending = (Bits & 0x80000000u) != 0 ? ~Bits : (Bits | 0x80000000u);
		return ~Ascending;
	}

	void Sort(std::span<const FKey> Keys, std::vector<uint32>& OutOrder)
	{
		const size_t Count = Keys.size();
		OutOrder.resize(Count);
		if (Count == 0)
		{
			return;
		}

		// 항목 = (깊이, 순번, 레이어, 번호) 부호 없는 정렬 비트. 처음 순서 = 제출 순서 → 안정 LSD라 동률은 제출 순서로 남는다
		struct FEntry
		{
			uint32 Words[3]; // [0] 깊이(내림차순 비트), [1] 순번, [2] 레이어 — 낮은 단어부터 정렬
			uint32 Index;
		};
		std::vector<FEntry> Entries(Count);
		std::vector<FEntry> Temp(Count);
		constexpr uint32    PassCount = 12; // 단어 3개 × 바이트 4개
		std::vector<std::array<uint32, 256>> Histograms(PassCount);
		for (std::array<uint32, 256>& Histogram : Histograms)
		{
			Histogram.fill(0);
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			FEntry& Entry  = Entries[Index];
			Entry.Words[0] = DepthToDescendingBits(Keys[Index].Depth);
			Entry.Words[1] = static_cast<uint32>(Keys[Index].Order) ^ 0x80000000u;
			Entry.Words[2] = static_cast<uint32>(Keys[Index].Layer) ^ 0x80000000u;
			Entry.Index    = static_cast<uint32>(Index);
			for (uint32 Pass = 0; Pass < PassCount; ++Pass)
			{
				++Histograms[Pass][(Entry.Words[Pass / 4] >> ((Pass % 4) * 8)) & 0xFFu];
			}
		}

		FEntry* Source = Entries.data();
		FEntry* Dest   = Temp.data();
		for (uint32 Pass = 0; Pass < PassCount; ++Pass)
		{
			const uint32 Word  = Pass / 4;
			const uint32 Shift = (Pass % 4) * 8;
			std::array<uint32, 256>& Histogram = Histograms[Pass];
			// 모든 항목이 같은 바이트면 순서가 바뀌지 않는다 → 건너뜀
			if (Histogram[(Source[0].Words[Word] >> Shift) & 0xFFu] == Count)
			{
				continue;
			}
			uint32 Offset = 0;
			for (uint32& Bucket : Histogram)
			{
				const uint32 BucketCount = Bucket;
				Bucket                   = Offset;
				Offset += BucketCount;
			}
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const FEntry& Entry = Source[Index];
				Dest[Histogram[(Entry.Words[Word] >> Shift) & 0xFFu]++] = Entry;
			}
			std::swap(Source, Dest);
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			OutOrder[Index] = Source[Index].Index;
		}
	}
} // namespace SpriteSorting

namespace SpriteBatching
{
	void BuildRuns(std::span<const uint8> PipelineKeys, std::vector<FRun>& OutRuns)
	{
		BuildRuns(PipelineKeys, {}, OutRuns);
	}

	void BuildRuns(std::span<const uint8> PipelineKeys, std::span<const int32> ChunkIndices, std::vector<FRun>& OutRuns)
	{
		OutRuns.clear();
		uint32 ItemIndex = 0;
		for (size_t Index = 0; Index < PipelineKeys.size(); ++Index)
		{
			const uint32 Key = PipelineKeys[Index];
			if (Index < ChunkIndices.size() && ChunkIndices[Index] >= 0)
			{
				OutRuns.push_back({ static_cast<uint32>(ChunkIndices[Index]), 0u, Key, true });
				continue;
			}
			if (OutRuns.empty() || OutRuns.back().bChunk || OutRuns.back().PipelineKey != Key)
			{
				OutRuns.push_back({ ItemIndex, 0u, Key, false });
			}
			++OutRuns.back().Count;
			++ItemIndex;
		}
	}
} // namespace SpriteBatching

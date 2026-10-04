#include "Renderer/SpriteDraw.h"

#include <array>
#include <bit>
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

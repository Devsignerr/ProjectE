#include "Scene/Terrain.h"

#include "Core/FileSystem.h"
#include "Core/Paths.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <fstream>

E_DEFINE_LOG_CATEGORY(LogTerrain, Log)

namespace
{
	using nlohmann::json;

	constexpr size_t MaxRecentChanges = 64;

	float ClampGrid(float Value, int32 Resolution)
	{
		return FMath::Clamp(Value, 0.0f, static_cast<float>(Resolution - 1));
	}

	uint16 ToHeight16(float Value)
	{
		return static_cast<uint16>(FMath::Clamp(std::lround(Value), 0L, 65535L));
	}

	uint32 Hash2(int32 X, int32 Y, uint32 Seed)
	{
		uint32 H = static_cast<uint32>(X) * 0x8DA6B343u ^ static_cast<uint32>(Y) * 0xD8163841u ^ Seed * 0xCB1AB31Fu;
		H ^= H >> 13;
		H *= 0x5BD1E995u;
		H ^= H >> 15;
		return H;
	}

	// 브러시 영향 정점 사각형 (지형 밖은 잘림)
	FTerrainRect GetBrushRect(const FTerrainFrame& Frame, const FVector2& WorldCenter, float Radius)
	{
		const FVector2 Min = Frame.WorldToGrid(WorldCenter.X - Radius, WorldCenter.Y - Radius);
		const FVector2 Max = Frame.WorldToGrid(WorldCenter.X + Radius, WorldCenter.Y + Radius);
		FTerrainRect   Rect{ static_cast<int32>(std::floor(Min.X)), static_cast<int32>(std::floor(Min.Y)), static_cast<int32>(std::ceil(Max.X)),
                           static_cast<int32>(std::ceil(Max.Y)) };
		return Rect.Clipped(Frame.Resolution);
	}

	// ---- PNG (16비트 회색조, 무압축 deflate)
	std::array<uint32, 256> MakeCrcTable()
	{
		std::array<uint32, 256> Table{};
		for (uint32 Index = 0; Index < 256; ++Index)
		{
			uint32 C = Index;
			for (int32 Bit = 0; Bit < 8; ++Bit)
			{
				C = (C & 1u) ? 0xEDB88320u ^ (C >> 1) : C >> 1;
			}
			Table[Index] = C;
		}
		return Table;
	}

	uint32 Crc32(const uint8* Data, size_t Size, uint32 Crc = 0xFFFFFFFFu)
	{
		static const std::array<uint32, 256> Table = MakeCrcTable();
		for (size_t Index = 0; Index < Size; ++Index)
		{
			Crc = Table[(Crc ^ Data[Index]) & 0xFFu] ^ (Crc >> 8);
		}
		return Crc;
	}

	void PutBigEndian32(std::vector<uint8>& Out, uint32 Value)
	{
		Out.push_back(static_cast<uint8>(Value >> 24));
		Out.push_back(static_cast<uint8>(Value >> 16));
		Out.push_back(static_cast<uint8>(Value >> 8));
		Out.push_back(static_cast<uint8>(Value));
	}

	void PutPngChunk(std::vector<uint8>& Out, const char Type[4], const std::vector<uint8>& Payload)
	{
		PutBigEndian32(Out, static_cast<uint32>(Payload.size()));
		const size_t TypeStart = Out.size();
		Out.insert(Out.end(), Type, Type + 4);
		Out.insert(Out.end(), Payload.begin(), Payload.end());
		const uint32 Crc = Crc32(Out.data() + TypeStart, Out.size() - TypeStart) ^ 0xFFFFFFFFu;
		PutBigEndian32(Out, Crc);
	}

	// 필터 바이트가 붙은 스캔라인 → PNG (무압축 deflate 블록 + Adler-32). ColorType 0 = 회색조, 6 = RGBA
	std::vector<uint8> EncodePngScanlines(const std::vector<uint8>& Raw, uint32 Width, uint32 Height, uint8 BitDepth, uint8 ColorType)
	{
		std::vector<uint8> Out = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
		std::vector<uint8> Header;
		PutBigEndian32(Header, Width);
		PutBigEndian32(Header, Height);
		Header.push_back(BitDepth);
		Header.push_back(ColorType);
		Header.push_back(0); // 압축
		Header.push_back(0); // 필터
		Header.push_back(0); // 비월 없음
		PutPngChunk(Out, "IHDR", Header);

		std::vector<uint8> Zlib = { 0x78, 0x01 };
		uint32             A    = 1;
		uint32             B    = 0;
		for (const uint8 Byte : Raw)
		{
			A = (A + Byte) % 65521u;
			B = (B + A) % 65521u;
		}
		size_t Offset = 0;
		do
		{
			const size_t Chunk  = std::min<size_t>(Raw.size() - Offset, 65535);
			const bool   bFinal = Offset + Chunk >= Raw.size();
			Zlib.push_back(bFinal ? 1 : 0);
			const uint16 Length = static_cast<uint16>(Chunk);
			Zlib.push_back(static_cast<uint8>(Length & 0xFFu));
			Zlib.push_back(static_cast<uint8>(Length >> 8));
			Zlib.push_back(static_cast<uint8>(~Length & 0xFFu));
			Zlib.push_back(static_cast<uint8>((~Length >> 8) & 0xFFu));
			Zlib.insert(Zlib.end(), Raw.begin() + static_cast<ptrdiff_t>(Offset), Raw.begin() + static_cast<ptrdiff_t>(Offset + Chunk));
			Offset += Chunk;
		} while (Offset < Raw.size());
		PutBigEndian32(Zlib, (B << 16) | A);
		PutPngChunk(Out, "IDAT", Zlib);
		PutPngChunk(Out, "IEND", {});
		return Out;
	}

	constexpr char Base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

// ---- FTerrainRect -------------------------------------------------------------------------------------------------

void FTerrainRect::Add(const FTerrainRect& Other)
{
	if (Other.IsEmpty())
	{
		return;
	}
	if (IsEmpty())
	{
		*this = Other;
		return;
	}
	MinX = std::min(MinX, Other.MinX);
	MinY = std::min(MinY, Other.MinY);
	MaxX = std::max(MaxX, Other.MaxX);
	MaxY = std::max(MaxY, Other.MaxY);
}

FTerrainRect FTerrainRect::Clipped(int32 Resolution) const
{
	FTerrainRect Result{ std::max(MinX, 0), std::max(MinY, 0), std::min(MaxX, Resolution - 1), std::min(MaxY, Resolution - 1) };
	return Result;
}

// ---- FTerrainData -------------------------------------------------------------------------------------------------

void FTerrainData::Initialize(uint32 InResolution)
{
	Resolution = std::max(InResolution, 2u);
	Heights.assign(static_cast<size_t>(Resolution) * Resolution, DefaultHeight);
	Weights.assign(Heights.size(), 255u);
	Revision = 0;
	DroppedUpTo = ChangeCounter; // 이전 내용을 본 쪽은 전체를 다시
	RecentChanges.clear();
	MarkChanged(FTerrainRect::Full(static_cast<int32>(Resolution)));
}

void FTerrainData::MarkChanged(const FTerrainRect& Rect)
{
	if (Rect.IsEmpty())
	{
		return;
	}
	// 모든 데이터 객체가 함께 쓰는 단조 증가 번호: 해제된 데이터 주소가 재사용돼도 렌더러/물리 캐시가 옛 번호로 착각하지 않는다
	static std::atomic<uint64> GChangeCounter{ 1 };
	ChangeCounter = ++GChangeCounter;
	bUnsaved      = true;
	if (RecentChanges.size() >= MaxRecentChanges)
	{
		DroppedUpTo = RecentChanges.front().Counter;
		RecentChanges.erase(RecentChanges.begin());
	}
	RecentChanges.push_back({ ChangeCounter, Rect.Clipped(static_cast<int32>(Resolution)) });
}

bool FTerrainData::GetChangesSince(uint64 Counter, FTerrainRect& OutRect) const
{
	OutRect = FTerrainRect{};
	if (Counter >= ChangeCounter)
	{
		return false;
	}
	// 기록에 Counter 바로 다음 변경이 없으면(버려졌거나 기록 전 상태) 전체를 다시
	// (번호는 모든 데이터 공용이라 이어지지 않는다 — 버린 기록 번호로 판단)
	if (RecentChanges.empty() || Counter < DroppedUpTo)
	{
		OutRect = FTerrainRect::Full(static_cast<int32>(Resolution));
		return true;
	}
	for (const FChange& Change : RecentChanges)
	{
		if (Change.Counter > Counter)
		{
			OutRect.Add(Change.Rect);
		}
	}
	return !OutRect.IsEmpty();
}

// ---- FTerrainFrame ------------------------------------------------------------------------------------------------

FTerrainFrame FTerrainFrame::Make(const FVector3& WorldPosition, const FTerrainComponent& Terrain, uint32 Resolution)
{
	FTerrainFrame Frame;
	Frame.Resolution       = static_cast<int32>(std::max(Resolution, 2u));
	const float Cells      = static_cast<float>(Frame.Resolution - 1);
	const FVector2 Size(std::max(Terrain.Size.X, 1.0f), std::max(Terrain.Size.Y, 1.0f));
	Frame.CellSize         = FVector2(Size.X / Cells, Size.Y / Cells);
	Frame.HeightScale      = std::max(Terrain.HeightRange, 1.0f) / 65535.0f;
	Frame.Origin           = FVector3(WorldPosition.X - Size.X * 0.5f, WorldPosition.Y - Size.Y * 0.5f,
                            WorldPosition.Z - std::max(Terrain.HeightRange, 1.0f) * 0.5f);
	return Frame;
}

FVector2 FTerrainFrame::WorldToGrid(float WorldX, float WorldY) const
{
	return FVector2((WorldX - Origin.X) / CellSize.X, (WorldY - Origin.Y) / CellSize.Y);
}

FVector3 FTerrainFrame::GridToWorld(float GridX, float GridY, float Height16) const
{
	return FVector3(Origin.X + GridX * CellSize.X, Origin.Y + GridY * CellSize.Y, HeightToWorldZ(Height16));
}

FBox FTerrainFrame::GetBounds(const FTerrainData& Data) const
{
	uint16 MinHeight = 65535;
	uint16 MaxHeight = 0;
	for (const uint16 Height : Data.Heights)
	{
		MinHeight = std::min(MinHeight, Height);
		MaxHeight = std::max(MaxHeight, Height);
	}
	if (Data.Heights.empty())
	{
		return GetFullBounds();
	}
	const float Last = static_cast<float>(Resolution - 1);
	return FBox(GridToWorld(0.0f, 0.0f, MinHeight), GridToWorld(Last, Last, MaxHeight));
}

FBox FTerrainFrame::GetFullBounds() const
{
	const float Last = static_cast<float>(Resolution - 1);
	return FBox(GridToWorld(0.0f, 0.0f, 0.0f), GridToWorld(Last, Last, 65535.0f));
}

// ---- TerrainMath --------------------------------------------------------------------------------------------------

float TerrainMath::SampleHeight(const FTerrainData& Data, float GridX, float GridY)
{
	const int32 Resolution = static_cast<int32>(Data.Resolution);
	GridX                  = ClampGrid(GridX, Resolution);
	GridY                  = ClampGrid(GridY, Resolution);
	const int32 X0         = std::min(static_cast<int32>(GridX), Resolution - 2);
	const int32 Y0         = std::min(static_cast<int32>(GridY), Resolution - 2);
	const float FX         = GridX - static_cast<float>(X0);
	const float FY         = GridY - static_cast<float>(Y0);
	const float H00        = Data.GetHeight(X0, Y0);
	const float H10        = Data.GetHeight(X0 + 1, Y0);
	const float H01        = Data.GetHeight(X0, Y0 + 1);
	const float H11        = Data.GetHeight(X0 + 1, Y0 + 1);
	// 대각선 (0,0)-(1,1)로 나눈 두 삼각형
	if (FX >= FY)
	{
		return H00 + FX * (H10 - H00) + FY * (H11 - H10);
	}
	return H00 + FY * (H01 - H00) + FX * (H11 - H01);
}

float TerrainMath::SampleWorldHeight(const FTerrainData& Data, const FTerrainFrame& Frame, float WorldX, float WorldY)
{
	const FVector2 Grid = Frame.WorldToGrid(WorldX, WorldY);
	return Frame.HeightToWorldZ(SampleHeight(Data, Grid.X, Grid.Y));
}

FVector3 TerrainMath::ComputeNormal(const FTerrainData& Data, const FTerrainFrame& Frame, float GridX, float GridY)
{
	const float Left  = Frame.HeightToWorldZ(SampleHeight(Data, GridX - 1.0f, GridY));
	const float Right = Frame.HeightToWorldZ(SampleHeight(Data, GridX + 1.0f, GridY));
	const float Down  = Frame.HeightToWorldZ(SampleHeight(Data, GridX, GridY - 1.0f));
	const float Up    = Frame.HeightToWorldZ(SampleHeight(Data, GridX, GridY + 1.0f));
	// 높이장 z = f(x, y)의 법선 = (-df/dx, -df/dy, 1)
	const FVector3 Normal(-(Right - Left) / (2.0f * Frame.CellSize.X), -(Up - Down) / (2.0f * Frame.CellSize.Y), 1.0f);
	return Normal.GetNormalized();
}

bool TerrainMath::Raycast(const FTerrainData& Data, const FTerrainFrame& Frame, const FVector3& Origin, const FVector3& Direction, float MaxDistance,
                          float& OutDistance)
{
	if (!Data.IsValid())
	{
		return false;
	}
	const FVector3 Dir = Direction.GetNormalized();
	if (Dir.IsNearlyZero())
	{
		return false;
	}
	// 경계 상자 구간으로 자른다
	const FBox Bounds = Frame.GetBounds(Data);
	float      TMin   = 0.0f;
	float      TMax   = MaxDistance;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float O = Origin[Axis];
		const float D = Dir[Axis];
		if (FMath::Abs(D) < 1.0e-8f)
		{
			if (O < Bounds.Min[Axis] || O > Bounds.Max[Axis])
			{
				return false;
			}
			continue;
		}
		float T0 = (Bounds.Min[Axis] - O) / D;
		float T1 = (Bounds.Max[Axis] - O) / D;
		if (T0 > T1)
		{
			std::swap(T0, T1);
		}
		TMin = std::max(TMin, T0);
		TMax = std::min(TMax, T1);
		if (TMin > TMax)
		{
			return false;
		}
	}

	auto Above = [&](float T) {
		const FVector3 P = Origin + Dir * T;
		return P.Z - SampleWorldHeight(Data, Frame, P.X, P.Y);
	};
	const float Step     = 0.5f * std::min(Frame.CellSize.X, Frame.CellSize.Y);
	float       Previous = TMin;
	float       PrevGap  = Above(TMin);
	if (PrevGap <= 0.0f)
	{
		OutDistance = TMin; // 지형 아래(또는 위에 딱 붙어) 시작
		return true;
	}
	for (float T = TMin + Step;; T += Step)
	{
		const float Current = std::min(T, TMax);
		const float Gap     = Above(Current);
		if (Gap <= 0.0f)
		{
			// 이분 탐색으로 다듬기
			float Low  = Previous;
			float High = Current;
			for (int32 Iteration = 0; Iteration < 12; ++Iteration)
			{
				const float Mid = 0.5f * (Low + High);
				(Above(Mid) > 0.0f ? Low : High) = Mid;
			}
			OutDistance = 0.5f * (Low + High);
			return true;
		}
		if (Current >= TMax)
		{
			return false;
		}
		Previous = Current;
		PrevGap  = Gap;
	}
}

float TerrainMath::BrushFalloff(float NormalizedDistance, float Falloff)
{
	if (NormalizedDistance >= 1.0f)
	{
		return 0.0f;
	}
	Falloff               = FMath::Clamp(Falloff, 0.0f, 1.0f);
	const float SolidPart = 1.0f - Falloff;
	if (NormalizedDistance <= SolidPart || Falloff <= 0.0f)
	{
		return 1.0f;
	}
	const float T = (NormalizedDistance - SolidPart) / Falloff; // 0 → 1
	const float S = 1.0f - T;
	return S * S * (3.0f - 2.0f * S); // smoothstep
}

uint8 TerrainMath::GetLayerWeight(uint32 Packed, uint32 Layer)
{
	return static_cast<uint8>((Packed >> (Layer * 8)) & 0xFFu);
}

uint32 TerrainMath::NormalizeWeight(uint32 Packed)
{
	float Values[TerrainMaxLayers];
	float Sum = 0.0f;
	for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
	{
		Values[Layer] = static_cast<float>(GetLayerWeight(Packed, Layer));
		Sum += Values[Layer];
	}
	if (Sum <= 0.0f)
	{
		return 255u;
	}
	uint32 Result = 0;
	int32  Total  = 0;
	int32  Bytes[TerrainMaxLayers];
	uint32 Largest = 0;
	for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
	{
		Bytes[Layer] = static_cast<int32>(std::lround(Values[Layer] * 255.0f / Sum));
		Total += Bytes[Layer];
		if (Values[Layer] > Values[Largest])
		{
			Largest = Layer;
		}
	}
	Bytes[Largest] = FMath::Clamp(Bytes[Largest] + (255 - Total), 0, 255); // 반올림 오차는 가장 큰 레이어가 흡수
	for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
	{
		Result |= static_cast<uint32>(Bytes[Layer]) << (Layer * 8);
	}
	return Result;
}

uint32 TerrainMath::PaintWeight(uint32 Packed, uint32 Layer, float Alpha)
{
	Layer = std::min(Layer, TerrainMaxLayers - 1);
	Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
	float Values[TerrainMaxLayers];
	for (uint32 Index = 0; Index < TerrainMaxLayers; ++Index)
	{
		Values[Index] = static_cast<float>(GetLayerWeight(Packed, Index));
	}
	const float Old    = Values[Layer];
	const float New    = Old + (255.0f - Old) * Alpha;
	const float Others = 255.0f - Old;
	const float Scale  = Others > 0.0f ? (255.0f - New) / Others : 0.0f;
	for (uint32 Index = 0; Index < TerrainMaxLayers; ++Index)
	{
		Values[Index] = Index == Layer ? New : Values[Index] * Scale;
	}
	uint32 Result = 0;
	int32  Total  = 0;
	for (uint32 Index = 0; Index < TerrainMaxLayers; ++Index)
	{
		const int32 Byte = FMath::Clamp(static_cast<int32>(std::lround(Values[Index])), 0, 255);
		Total += Byte;
		Result |= static_cast<uint32>(Byte) << (Index * 8);
	}
	if (Total != 255)
	{
		// 반올림 오차는 칠하는 레이어가 흡수
		const int32 Fixed = FMath::Clamp(static_cast<int32>(GetLayerWeight(Result, Layer)) + (255 - Total), 0, 255);
		Result            = (Result & ~(0xFFu << (Layer * 8))) | (static_cast<uint32>(Fixed) << (Layer * 8));
	}
	return Result;
}

float TerrainMath::ValueNoise(float X, float Y, uint32 Seed)
{
	const float FloorX = std::floor(X);
	const float FloorY = std::floor(Y);
	const int32 IX     = static_cast<int32>(FloorX);
	const int32 IY     = static_cast<int32>(FloorY);
	float       FX     = X - FloorX;
	float       FY     = Y - FloorY;
	FX                 = FX * FX * (3.0f - 2.0f * FX);
	FY                 = FY * FY * (3.0f - 2.0f * FY);
	auto Corner = [&](int32 CX, int32 CY) { return static_cast<float>(Hash2(CX, CY, Seed) & 0xFFFFu) / 65535.0f; };
	const float A = FMath::Lerp(Corner(IX, IY), Corner(IX + 1, IY), FX);
	const float B = FMath::Lerp(Corner(IX, IY + 1), Corner(IX + 1, IY + 1), FX);
	return FMath::Lerp(A, B, FY);
}

// ---- 브러시 -------------------------------------------------------------------------------------------------------

FTerrainRect ApplyTerrainBrush(FTerrainData& Data, const FTerrainFrame& Frame, const FTerrainBrush& Brush, const FVector2& WorldCenter, float DeltaSeconds)
{
	if (!Data.IsValid() || Brush.Radius <= 0.0f || DeltaSeconds <= 0.0f || Brush.Strength <= 0.0f)
	{
		return {};
	}
	const FTerrainRect Rect = GetBrushRect(Frame, WorldCenter, Brush.Radius);
	if (Rect.IsEmpty())
	{
		return {};
	}
	const int32 Resolution = static_cast<int32>(Data.Resolution);
	const float Strength   = FMath::Clamp(Brush.Strength, 0.0f, 1.0f);

	// 올리기/내리기 속도: 세기 1 = 초당 10m
	constexpr float RaiseSpeed = 1000.0f; // cm/s
	const float     RaiseUnits = RaiseSpeed * DeltaSeconds / Frame.HeightScale;
	const float     BlendRate  = std::min(1.0f, 8.0f * DeltaSeconds); // 평탄화/부드럽게/칠하기의 초당 다가가는 비율
	const float     FlattenH   = Frame.WorldZToHeight(Brush.FlattenHeight);

	// 부드럽게: 원본 영역 복사 (이웃 평균이 이번 적용 결과에 섞이지 않게)
	std::vector<uint16> Source;
	if (Brush.Op == ETerrainBrushOp::Smooth)
	{
		Source = Data.Heights;
	}

	FTerrainRect Changed;
	for (int32 Y = Rect.MinY; Y <= Rect.MaxY; ++Y)
	{
		for (int32 X = Rect.MinX; X <= Rect.MaxX; ++X)
		{
			const FVector3 World = Frame.GridToWorld(static_cast<float>(X), static_cast<float>(Y), 0.0f);
			const float    DX    = World.X - WorldCenter.X;
			const float    DY    = World.Y - WorldCenter.Y;
			const float    Dist  = std::sqrt(DX * DX + DY * DY) / Brush.Radius;
			const float    W     = TerrainMath::BrushFalloff(Dist, Brush.Falloff) * Strength;
			if (W <= 0.0f)
			{
				continue;
			}
			const size_t Index = static_cast<size_t>(Y) * Resolution + X;
			if (Brush.Op == ETerrainBrushOp::Paint)
			{
				const uint32 Old = Data.Weights[Index];
				const uint32 New = TerrainMath::PaintWeight(Old, Brush.Layer, W * BlendRate * 2.0f);
				if (New != Old)
				{
					Data.Weights[Index] = New;
					Changed.Add({ X, Y, X, Y });
				}
				continue;
			}

			const float Old = static_cast<float>(Data.Heights[Index]);
			float       New = Old;
			switch (Brush.Op)
			{
			case ETerrainBrushOp::Raise: New = Old + W * RaiseUnits; break;
			case ETerrainBrushOp::Lower: New = Old - W * RaiseUnits; break;
			case ETerrainBrushOp::Flatten: New = Old + (FlattenH - Old) * std::min(1.0f, W * BlendRate * 2.0f); break;
			case ETerrainBrushOp::Smooth:
			{
				float Sum   = 0.0f;
				int32 Count = 0;
				for (int32 OY = -1; OY <= 1; ++OY)
				{
					for (int32 OX = -1; OX <= 1; ++OX)
					{
						const int32 NX = X + OX;
						const int32 NY = Y + OY;
						if (NX >= 0 && NY >= 0 && NX < Resolution && NY < Resolution)
						{
							Sum += Source[static_cast<size_t>(NY) * Resolution + NX];
							++Count;
						}
					}
				}
				New = Old + (Sum / static_cast<float>(Count) - Old) * std::min(1.0f, W * BlendRate * 2.0f);
				break;
			}
			case ETerrainBrushOp::Noise:
			{
				const float Scale = std::max(Brush.NoiseScale, 1.0f);
				const float N     = TerrainMath::ValueNoise(World.X / Scale, World.Y / Scale, Brush.Seed) * 2.0f - 1.0f;
				New               = Old + N * W * RaiseUnits * 0.5f;
				break;
			}
			default: break;
			}
			const uint16 Quantized = ToHeight16(New);
			if (Quantized != Data.Heights[Index])
			{
				Data.Heights[Index] = Quantized;
				Changed.Add({ X, Y, X, Y });
			}
		}
	}
	return Changed;
}

// ---- FTerrainRegion -----------------------------------------------------------------------------------------------

FTerrainRegion FTerrainRegion::Capture(const FTerrainData& Data, const FTerrainRect& Rect)
{
	return CaptureFrom(Data.Heights, Data.Weights, Data.Resolution, Rect);
}

FTerrainRegion FTerrainRegion::CaptureFrom(const std::vector<uint16>& Heights, const std::vector<uint32>& Weights, uint32 Resolution, const FTerrainRect& Rect)
{
	FTerrainRegion Region;
	Region.Rect = Rect.Clipped(static_cast<int32>(Resolution));
	if (Region.Rect.IsEmpty())
	{
		return Region;
	}
	const size_t Count = static_cast<size_t>(Region.Rect.GetWidth()) * Region.Rect.GetHeight();
	Region.Heights.reserve(Count);
	Region.Weights.reserve(Count);
	for (int32 Y = Region.Rect.MinY; Y <= Region.Rect.MaxY; ++Y)
	{
		const size_t Row = static_cast<size_t>(Y) * Resolution;
		Region.Heights.insert(Region.Heights.end(), Heights.begin() + static_cast<ptrdiff_t>(Row + Region.Rect.MinX),
		                      Heights.begin() + static_cast<ptrdiff_t>(Row + Region.Rect.MaxX + 1));
		Region.Weights.insert(Region.Weights.end(), Weights.begin() + static_cast<ptrdiff_t>(Row + Region.Rect.MinX),
		                      Weights.begin() + static_cast<ptrdiff_t>(Row + Region.Rect.MaxX + 1));
	}
	return Region;
}

void FTerrainRegion::Apply(FTerrainData& Data) const
{
	if (Rect.IsEmpty() || Rect.MaxX >= static_cast<int32>(Data.Resolution) || Rect.MaxY >= static_cast<int32>(Data.Resolution))
	{
		return;
	}
	const size_t Width = static_cast<size_t>(Rect.GetWidth());
	for (int32 Y = Rect.MinY; Y <= Rect.MaxY; ++Y)
	{
		const size_t Row    = static_cast<size_t>(Y) * Data.Resolution + Rect.MinX;
		const size_t Source = static_cast<size_t>(Y - Rect.MinY) * Width;
		std::copy_n(Heights.begin() + static_cast<ptrdiff_t>(Source), Width, Data.Heights.begin() + static_cast<ptrdiff_t>(Row));
		std::copy_n(Weights.begin() + static_cast<ptrdiff_t>(Source), Width, Data.Weights.begin() + static_cast<ptrdiff_t>(Row));
	}
}

// ---- TerrainIO ----------------------------------------------------------------------------------------------------

std::string TerrainIO::EncodeBase64(const uint8* Data, size_t Size)
{
	std::string Out;
	Out.reserve((Size + 2) / 3 * 4);
	for (size_t Index = 0; Index < Size; Index += 3)
	{
		const uint32 B0 = Data[Index];
		const uint32 B1 = Index + 1 < Size ? Data[Index + 1] : 0u;
		const uint32 B2 = Index + 2 < Size ? Data[Index + 2] : 0u;
		const uint32 V  = (B0 << 16) | (B1 << 8) | B2;
		Out.push_back(Base64Alphabet[(V >> 18) & 63]);
		Out.push_back(Base64Alphabet[(V >> 12) & 63]);
		Out.push_back(Index + 1 < Size ? Base64Alphabet[(V >> 6) & 63] : '=');
		Out.push_back(Index + 2 < Size ? Base64Alphabet[V & 63] : '=');
	}
	return Out;
}

bool TerrainIO::DecodeBase64(std::string_view Text, std::vector<uint8>& OutBytes)
{
	int8 Lookup[256];
	std::memset(Lookup, -1, sizeof(Lookup));
	for (int32 Index = 0; Index < 64; ++Index)
	{
		Lookup[static_cast<uint8>(Base64Alphabet[Index])] = static_cast<int8>(Index);
	}
	OutBytes.clear();
	OutBytes.reserve(Text.size() / 4 * 3);
	uint32 Accumulator = 0;
	int32  Bits        = 0;
	for (const char Char : Text)
	{
		if (Char == '=')
		{
			break;
		}
		const int8 Value = Lookup[static_cast<uint8>(Char)];
		if (Value < 0)
		{
			if (Char == '\n' || Char == '\r' || Char == ' ')
			{
				continue;
			}
			return false;
		}
		Accumulator = (Accumulator << 6) | static_cast<uint32>(Value);
		Bits += 6;
		if (Bits >= 8)
		{
			Bits -= 8;
			OutBytes.push_back(static_cast<uint8>((Accumulator >> Bits) & 0xFFu));
		}
	}
	return true;
}

std::string TerrainIO::ToJsonString(const FTerrainData& Data)
{
	json Root;
	Root["Version"]    = Version;
	Root["Resolution"] = Data.Resolution;
	Root["Revision"]   = Data.Revision;
	// 높이: uint16 리틀 엔디언, 가중치: 정점마다 RGBA8 (레이어 0~3)
	Root["Heights"] = EncodeBase64(reinterpret_cast<const uint8*>(Data.Heights.data()), Data.Heights.size() * sizeof(uint16));
	Root["Weights"] = EncodeBase64(reinterpret_cast<const uint8*>(Data.Weights.data()), Data.Weights.size() * sizeof(uint32));
	return Root.dump(1, '\t') + "\n";
}

bool TerrainIO::FromJsonString(std::string_view Text, FTerrainData& OutData, std::string* OutError)
{
	auto Fail = [&](const std::string& Message) {
		if (OutError != nullptr)
		{
			*OutError = Message;
		}
		return false;
	};
	const json Root = json::parse(Text.begin(), Text.end(), nullptr, false);
	if (!Root.is_object())
	{
		return Fail("JSON 형식 오류");
	}
	const uint32 Resolution = Root.value("Resolution", 0u);
	if (Resolution < 2 || Resolution > 8193)
	{
		return Fail("해상도가 잘못됨");
	}
	std::vector<uint8> HeightBytes;
	std::vector<uint8> WeightBytes;
	if (!DecodeBase64(Root.value("Heights", std::string()), HeightBytes) || !DecodeBase64(Root.value("Weights", std::string()), WeightBytes))
	{
		return Fail("base64 오류");
	}
	const size_t Count = static_cast<size_t>(Resolution) * Resolution;
	if (HeightBytes.size() != Count * sizeof(uint16))
	{
		return Fail("높이 데이터 크기가 맞지 않음");
	}
	FTerrainData Data;
	Data.Resolution = Resolution;
	Data.Revision   = Root.value("Revision", 0u);
	Data.Heights.resize(Count);
	std::memcpy(Data.Heights.data(), HeightBytes.data(), HeightBytes.size());
	if (WeightBytes.size() == Count * sizeof(uint32))
	{
		Data.Weights.resize(Count);
		std::memcpy(Data.Weights.data(), WeightBytes.data(), WeightBytes.size());
	}
	else
	{
		Data.Weights.assign(Count, 255u); // 가중치가 없거나 깨졌으면 레이어 0
	}
	Data.MarkChanged(FTerrainRect::Full(static_cast<int32>(Resolution)));
	Data.bUnsaved = false;
	OutData       = std::move(Data);
	return true;
}

bool TerrainIO::SaveToFile(const FTerrainData& Data, const std::filesystem::path& Path)
{
	std::error_code ErrorCode;
	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
	}
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogTerrain, Error, "지형 저장 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	const std::string Text = ToJsonString(Data);
	File.write(Text.data(), static_cast<std::streamsize>(Text.size()));
	return File.good();
}

bool TerrainIO::LoadFromFile(const std::filesystem::path& Path, FTerrainData& OutData, std::string* OutError)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		if (OutError != nullptr)
		{
			*OutError = "파일을 읽을 수 없음";
		}
		return false;
	}
	return FromJsonString(Text, OutData, OutError);
}

std::vector<uint16> TerrainIO::ResampleHeights(const std::vector<uint16>& Source, uint32 SourceWidth, uint32 SourceHeight, uint32 Resolution)
{
	std::vector<uint16> Result(static_cast<size_t>(Resolution) * Resolution, FTerrainData::DefaultHeight);
	if (SourceWidth < 1 || SourceHeight < 1 || Source.size() < static_cast<size_t>(SourceWidth) * SourceHeight || Resolution < 2)
	{
		return Result;
	}
	for (uint32 Y = 0; Y < Resolution; ++Y)
	{
		const float SY = static_cast<float>(Y) * static_cast<float>(SourceHeight - 1) / static_cast<float>(Resolution - 1);
		const uint32 Y0 = std::min(static_cast<uint32>(SY), SourceHeight - 1);
		const uint32 Y1 = std::min(Y0 + 1, SourceHeight - 1);
		const float  FY = SY - static_cast<float>(Y0);
		for (uint32 X = 0; X < Resolution; ++X)
		{
			const float SX = static_cast<float>(X) * static_cast<float>(SourceWidth - 1) / static_cast<float>(Resolution - 1);
			const uint32 X0 = std::min(static_cast<uint32>(SX), SourceWidth - 1);
			const uint32 X1 = std::min(X0 + 1, SourceWidth - 1);
			const float  FX = SX - static_cast<float>(X0);
			const float  A  = FMath::Lerp(static_cast<float>(Source[Y0 * SourceWidth + X0]), static_cast<float>(Source[Y0 * SourceWidth + X1]), FX);
			const float  B  = FMath::Lerp(static_cast<float>(Source[Y1 * SourceWidth + X0]), static_cast<float>(Source[Y1 * SourceWidth + X1]), FX);
			Result[static_cast<size_t>(Y) * Resolution + X] = ToHeight16(FMath::Lerp(A, B, FY));
		}
	}
	return Result;
}

bool TerrainIO::ReadRaw16(const std::filesystem::path& Path, std::vector<uint16>& OutHeights, uint32& OutSize)
{
	std::vector<uint8> Bytes;
	if (!FFileSystem::ReadFile(Path, Bytes) || Bytes.size() < 8 || Bytes.size() % 2 != 0)
	{
		return false;
	}
	const size_t Count = Bytes.size() / 2;
	const uint32 Side  = static_cast<uint32>(std::lround(std::sqrt(static_cast<double>(Count))));
	if (static_cast<size_t>(Side) * Side != Count)
	{
		return false; // 정사각형만
	}
	OutHeights.resize(Count);
	std::memcpy(OutHeights.data(), Bytes.data(), Bytes.size());
	OutSize = Side;
	return true;
}

bool TerrainIO::WriteRaw16(const std::filesystem::path& Path, const FTerrainData& Data)
{
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		return false;
	}
	File.write(reinterpret_cast<const char*>(Data.Heights.data()), static_cast<std::streamsize>(Data.Heights.size() * sizeof(uint16)));
	return File.good();
}

std::vector<uint8> TerrainIO::EncodePng16(const std::vector<uint16>& Pixels, uint32 Width, uint32 Height)
{
	// 원시 스캔라인 (필터 0 + 빅 엔디언 16비트)
	std::vector<uint8> Raw;
	Raw.reserve(static_cast<size_t>(Height) * (1 + Width * 2));
	for (uint32 Y = 0; Y < Height; ++Y)
	{
		Raw.push_back(0);
		for (uint32 X = 0; X < Width; ++X)
		{
			const uint16 Value = Pixels[static_cast<size_t>(Y) * Width + X];
			Raw.push_back(static_cast<uint8>(Value >> 8));
			Raw.push_back(static_cast<uint8>(Value & 0xFFu));
		}
	}
	return EncodePngScanlines(Raw, Width, Height, 16, 0);
}

std::vector<uint8> TerrainIO::EncodePngRgba8(const std::vector<uint32>& Pixels, uint32 Width, uint32 Height)
{
	std::vector<uint8> Raw;
	Raw.reserve(static_cast<size_t>(Height) * (1 + Width * 4));
	for (uint32 Y = 0; Y < Height; ++Y)
	{
		Raw.push_back(0);
		for (uint32 X = 0; X < Width; ++X)
		{
			const uint32 Value = Pixels[static_cast<size_t>(Y) * Width + X]; // 바이트 순서 R, G, B, A
			Raw.push_back(static_cast<uint8>(Value & 0xFFu));
			Raw.push_back(static_cast<uint8>((Value >> 8) & 0xFFu));
			Raw.push_back(static_cast<uint8>((Value >> 16) & 0xFFu));
			Raw.push_back(static_cast<uint8>(Value >> 24));
		}
	}
	return EncodePngScanlines(Raw, Width, Height, 8, 6);
}

bool TerrainIO::WritePng16(const std::filesystem::path& Path, const FTerrainData& Data)
{
	const std::vector<uint8> Bytes = EncodePng16(Data.Heights, Data.Resolution, Data.Resolution);
	std::ofstream            File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		return false;
	}
	File.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
	return File.good();
}

// ---- FTerrainLibrary ----------------------------------------------------------------------------------------------

FTerrainLibrary& FTerrainLibrary::Get()
{
	static FTerrainLibrary Instance;
	return Instance;
}

void FTerrainLibrary::SetContentDirectory(const std::filesystem::path& Directory)
{
	ContentDirectory = Directory;
}

std::filesystem::path FTerrainLibrary::ResolveAssetPath(const std::string& Asset) const
{
	const std::filesystem::path Path = FStringConv::ToWide(Asset);
	if (Path.is_absolute())
	{
		return Path;
	}
	if (!ContentDirectory.empty())
	{
		return ContentDirectory / Path;
	}
	return FPaths::HasProject() ? FPaths::GetProjectContentDirectory() / Path : Path;
}

std::shared_ptr<FTerrainData> FTerrainLibrary::Find(const std::string& Asset) const
{
	const auto Found = Cache.find(Asset);
	return Found != Cache.end() ? Found->second : nullptr;
}

std::shared_ptr<FTerrainData> FTerrainLibrary::Load(const std::string& Asset)
{
	if (Asset.empty())
	{
		return nullptr;
	}
	if (const auto Found = Cache.find(Asset); Found != Cache.end())
	{
		return Found->second;
	}
	auto        Data = std::make_shared<FTerrainData>();
	std::string Error;
	if (!TerrainIO::LoadFromFile(ResolveAssetPath(Asset), *Data, &Error))
	{
		E_LOG(LogTerrain, Warning, "지형 로드 실패 '{}': {}", Asset, Error);
		Cache[Asset] = nullptr; // 같은 실패를 매 프레임 반복하지 않는다
		return nullptr;
	}
	E_LOG(LogTerrain, Log, "지형 로드: {} ({}x{})", Asset, Data->Resolution, Data->Resolution);
	Cache[Asset] = Data;
	return Data;
}

std::shared_ptr<FTerrainData> FTerrainLibrary::Create(const std::string& Asset, uint32 Resolution)
{
	auto Data = std::make_shared<FTerrainData>();
	Data->Initialize(Resolution);
	Cache[Asset] = Data;
	if (!Save(Asset))
	{
		return nullptr;
	}
	return Data;
}

bool FTerrainLibrary::Save(const std::string& Asset)
{
	const std::shared_ptr<FTerrainData> Data = Find(Asset);
	if (!Data)
	{
		return false;
	}
	if (!TerrainIO::SaveToFile(*Data, ResolveAssetPath(Asset)))
	{
		return false;
	}
	Data->bUnsaved = false;
	E_LOG(LogTerrain, Log, "지형 저장: {}", Asset);
	return true;
}

void FTerrainLibrary::SaveAllUnsaved()
{
	for (const auto& [Asset, Data] : Cache)
	{
		if (Data && Data->bUnsaved)
		{
			Save(Asset);
		}
	}
}

std::string FTerrainLibrary::MakeAssetPath(const std::filesystem::path& AbsolutePath) const
{
	const std::filesystem::path Root     = !ContentDirectory.empty() ? ContentDirectory : (FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : std::filesystem::path());
	const std::filesystem::path Relative = Root.empty() ? std::filesystem::path() : AbsolutePath.lexically_normal().lexically_relative(Root.lexically_normal());
	if (Relative.empty() || Relative.native().starts_with(L".."))
	{
		return FStringConv::ToUtf8(AbsolutePath.generic_wstring());
	}
	return FStringConv::ToUtf8(Relative.generic_wstring());
}

void FTerrainLibrary::OnAssetMoved(const std::filesystem::path& From, const std::filesystem::path& To)
{
	const std::wstring FromKey = From.lexically_normal().generic_wstring();
	std::vector<std::pair<std::string, std::string>> Renames;
	for (const auto& [Asset, Data] : Cache)
	{
		const std::wstring Path = ResolveAssetPath(Asset).lexically_normal().generic_wstring();
		if (Path.size() < FromKey.size() || _wcsnicmp(Path.c_str(), FromKey.c_str(), FromKey.size()) != 0 ||
		    (Path.size() > FromKey.size() && Path[FromKey.size()] != L'/'))
		{
			continue; // 같은 파일이거나 옮긴 폴더 안이어야 한다
		}
		const std::filesystem::path NewPath = std::filesystem::path(To.generic_wstring() + Path.substr(FromKey.size()));
		Renames.emplace_back(Asset, MakeAssetPath(NewPath));
	}
	for (const auto& [Old, New] : Renames)
	{
		std::shared_ptr<FTerrainData> Data = Cache[Old];
		Cache.erase(Old);
		Cache[New] = std::move(Data);
	}
}

void FTerrainLibrary::Invalidate(const std::string& Asset)
{
	Cache.erase(Asset);
}

void FTerrainLibrary::Clear()
{
	Cache.clear();
}

// ---- 씬 -----------------------------------------------------------------------------------------------------------

void GatherTerrains(FScene& Scene, std::vector<FTerrainInstance>& OutTerrains)
{
	OutTerrains.clear();
	FTerrainLibrary& Library = FTerrainLibrary::Get();
	Scene.GetRegistry().View<FTransformComponent, FTerrainComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FTerrainComponent& Terrain) {
		const std::shared_ptr<FTerrainData> Data = Library.Load(Terrain.Asset);
		if (!Data || !Data->IsValid())
		{
			return;
		}
		FTerrainInstance& Instance = OutTerrains.emplace_back();
		Instance.Entity            = Entity;
		Instance.Component         = &Terrain;
		Instance.Data              = Data.get(); // 라이브러리가 소유 (캐시를 비우기 전까지 유효)
		Instance.Frame             = FTerrainFrame::Make(Transform.GetWorldPosition(), Terrain, Data->Resolution);
	});
}

void RegisterTerrainTypes()
{
	FTypeRegistry& Registry = FTypeRegistry::Get();
	if (Registry.IsRegistered<FTerrainComponent>())
	{
		return;
	}
	// 지형: 위치만 따른다 (회전/스케일 무시). 높이맵은 .eterrain, 편집은 지형 도구 패널
	Registry.RegisterType<FTerrainComponent>("TerrainComponent", "지형")
		.Property(&FTerrainComponent::Asset, "Asset", "지형 데이터", PF_ReadOnly).AssetFilter(".eterrain")
		.Property(&FTerrainComponent::Size, "Size", "크기 (cm)").Range(100.0f, 1000000.0f, 10.0f)
		.Property(&FTerrainComponent::HeightRange, "HeightRange", "높이 범위 (cm)").Range(10.0f, 200000.0f, 10.0f)
		.Tooltip("16비트 높이 전체가 덮는 범위. 엔티티 Z가 가운데")
		.Property(&FTerrainComponent::Layer0Material, "Layer0Material", "레이어 0 머티리얼").AssetFilter(".emat")
		.Property(&FTerrainComponent::Layer1Material, "Layer1Material", "레이어 1 머티리얼").AssetFilter(".emat")
		.Property(&FTerrainComponent::Layer2Material, "Layer2Material", "레이어 2 머티리얼").AssetFilter(".emat")
		.Property(&FTerrainComponent::Layer3Material, "Layer3Material", "레이어 3 머티리얼").AssetFilter(".emat")
		.Property(&FTerrainComponent::Layer0Tiling, "Layer0Tiling", "레이어 0 타일 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FTerrainComponent::Layer1Tiling, "Layer1Tiling", "레이어 1 타일 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FTerrainComponent::Layer2Tiling, "Layer2Tiling", "레이어 2 타일 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FTerrainComponent::Layer3Tiling, "Layer3Tiling", "레이어 3 타일 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FTerrainComponent::bCastShadows, "CastShadows", "그림자 드리우기")
		.Property(&FTerrainComponent::bCollision, "Collision", "충돌")
		.Property(&FTerrainComponent::EditRevision, "EditRevision", "편집 버전", PF_Hidden | PF_NoReplicate)
		.AsComponent();
}

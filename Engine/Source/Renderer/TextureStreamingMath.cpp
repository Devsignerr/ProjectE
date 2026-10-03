#include "Renderer/TextureStreamingMath.h"

#include "Core/Math/Math.h"
#include "Renderer/MaterialGraph.h"
#include "Renderer/MeshData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <utility>

namespace
{
	uint32 MipDimension(uint32 Size, uint32 Mip)
	{
		return std::max(1u, Size >> Mip);
	}

	uint32 FullMipCount(uint32 Width, uint32 Height)
	{
		uint32 Count = 1;
		for (uint32 Size = std::max(Width, Height); Size > 1; Size >>= 1)
		{
			++Count;
		}
		return Count;
	}

	template <typename T>
	T ReadLittle(const uint8* Data)
	{
		T Value{};
		std::memcpy(&Value, Data, sizeof(T));
		return Value;
	}
} // namespace

uint64 TextureStreamingMath::FPayloadLayout::GetRangeDataBytes(uint32 Top) const
{
	uint64 Bytes = 0;
	for (uint32 Mip = Top; Mip < MipCount && Mip < Mips.size(); ++Mip)
	{
		Bytes += Mips[Mip].DataSize;
	}
	return Bytes;
}

TextureStreamingMath::FPayloadLayout TextureStreamingMath::ComputePayloadLayout(ETextureFormat Format, bool bSRGB, uint32 Width, uint32 Height, uint32 MipCount)
{
	FPayloadLayout Layout;
	if (Width == 0 || Height == 0 || MipCount == 0 || MipCount > FullMipCount(Width, Height))
	{
		return Layout;
	}
	Layout.Format   = Format;
	Layout.bSRGB    = bSRGB;
	Layout.Width    = Width;
	Layout.Height   = Height;
	Layout.MipCount = MipCount;
	Layout.Mips.resize(MipCount);
	uint64 Offset = PayloadHeaderSize;
	for (uint32 Mip = 0; Mip < MipCount; ++Mip)
	{
		FMipRecord& Record  = Layout.Mips[Mip];
		Record.Width        = MipDimension(Width, Mip);
		Record.Height       = MipDimension(Height, Mip);
		Record.DataSize     = TextureCompression::GetMipDataSize(Format, Record.Width, Record.Height);
		Record.RecordOffset = Offset;
		Record.DataOffset   = Offset + MipRecordHeaderSize;
		Offset              = Record.DataOffset + Record.DataSize;
	}
	Layout.TotalSize = Offset;
	return Layout;
}

TextureStreamingMath::FPayloadLayout TextureStreamingMath::ParsePayloadProbe(const uint8* Data, size_t Size)
{
	if (Data == nullptr || Size < PayloadProbeSize)
	{
		return {};
	}
	const uint8  Format   = Data[0];
	const uint8  bSRGB    = Data[1];
	const uint32 MipCount = ReadLittle<uint32>(Data + 2);
	const uint32 Width    = ReadLittle<uint32>(Data + 6);
	const uint32 Height   = ReadLittle<uint32>(Data + 10);
	if (Format > static_cast<uint8>(ETextureFormat::BC4))
	{
		return {};
	}
	return ComputePayloadLayout(static_cast<ETextureFormat>(Format), bSRGB != 0, Width, Height, MipCount);
}

bool TextureStreamingMath::ParseMipRange(const FPayloadLayout& Layout, uint32 Top, const uint8* Data, size_t Size, std::vector<FTextureMip>& OutMips)
{
	OutMips.clear();
	if (!Layout.IsValid() || Top >= Layout.MipCount || Data == nullptr || Size != Layout.GetRangeReadSize(Top))
	{
		return false;
	}
	const uint64 Base = Layout.GetRangeReadOffset(Top);
	OutMips.resize(Layout.MipCount - Top);
	for (uint32 Mip = Top; Mip < Layout.MipCount; ++Mip)
	{
		const FMipRecord& Record = Layout.Mips[Mip];
		const uint8*      Header = Data + (Record.RecordOffset - Base);
		const uint32      Width  = ReadLittle<uint32>(Header);
		const uint32      Height = ReadLittle<uint32>(Header + 4);
		const uint32      Bytes  = ReadLittle<uint32>(Header + 8);
		if (Width != Record.Width || Height != Record.Height || Bytes != Record.DataSize)
		{
			OutMips.clear();
			return false; // 파일이 바뀌었거나 손상 (재임포트 등)
		}
		FTextureMip& Out = OutMips[Mip - Top];
		Out.Width        = Width;
		Out.Height       = Height;
		Out.Data.assign(Data + (Record.DataOffset - Base), Data + (Record.DataOffset - Base) + Record.DataSize);
	}
	return true;
}

uint32 TextureStreamingMath::GetMaxValidTopMip(ETextureFormat Format, uint32 Width, uint32 Height, uint32 MipCount)
{
	const bool bBlock = TextureCompression::GetBlockBytes(Format) > 0;
	uint32     Best   = 0;
	for (uint32 Mip = 1; Mip < MipCount; ++Mip)
	{
		const uint32 Divisor = 1u << Mip;
		if (Width % Divisor != 0 || Height % Divisor != 0)
		{
			break;
		}
		if (bBlock && ((Width >> Mip) % 4 != 0 || (Height >> Mip) % 4 != 0))
		{
			break;
		}
		Best = Mip;
	}
	return Best;
}

uint32 TextureStreamingMath::ComputeTailTopMip(ETextureFormat Format, uint32 Width, uint32 Height, uint32 MipCount, uint32 NonStreamingMax,
                                               uint32 MinResident)
{
	if (std::max(Width, Height) <= NonStreamingMax || MipCount <= 1)
	{
		return 0;
	}
	const uint32 MaxValid = GetMaxValidTopMip(Format, Width, Height, MipCount);
	for (uint32 Mip = 0; Mip <= MaxValid; ++Mip)
	{
		if (std::max(MipDimension(Width, Mip), MipDimension(Height, Mip)) <= MinResident)
		{
			return Mip;
		}
	}
	return MaxValid; // 꼬리 크기까지 내려갈 수 없는 크기(2의 거듭제곱이 아님)면 갈 수 있는 데까지
}

float TextureStreamingMath::ComputePerspectiveCmPerPixel(float ViewDepth, float TanHalfFovY, uint32 ScreenHeight)
{
	if (ViewDepth <= 0.0f || TanHalfFovY <= 0.0f || ScreenHeight == 0)
	{
		return 0.0f;
	}
	return 2.0f * ViewDepth * TanHalfFovY / static_cast<float>(ScreenHeight);
}

float TextureStreamingMath::ComputeOrthographicCmPerPixel(float OrthoHeight, uint32 ScreenHeight)
{
	if (OrthoHeight <= 0.0f || ScreenHeight == 0)
	{
		return 0.0f;
	}
	return OrthoHeight / static_cast<float>(ScreenHeight);
}

float TextureStreamingMath::ComputeLog2UvPerPixel(float UvDensityPerCm, float CmPerPixel)
{
	if (UvDensityPerCm <= 0.0f || CmPerPixel <= 0.0f)
	{
		return -std::numeric_limits<float>::infinity();
	}
	return std::log2(UvDensityPerCm * CmPerPixel);
}

uint32 TextureStreamingMath::ComputeRequiredTopMip(uint32 TextureMaxSize, float Log2UvPerPixel, float MipBias, int32 Margin, uint32 MipCount)
{
	if (MipCount == 0 || TextureMaxSize == 0 || !std::isfinite(Log2UvPerPixel))
	{
		return 0;
	}
	const float Lod = std::log2(static_cast<float>(TextureMaxSize)) + Log2UvPerPixel + MipBias;
	const float Top = std::floor(Lod) - static_cast<float>(Margin);
	if (!(Top > 0.0f))
	{
		return 0;
	}
	return std::min(static_cast<uint32>(std::min(Top, 64.0f)), MipCount - 1);
}

uint32 TextureStreamingMath::ComputeRequiredTopMip(uint32 TextureMaxSize, float UvDensityPerCm, float CmPerPixel, float MipBias, int32 Margin,
                                                   uint32 MipCount)
{
	return ComputeRequiredTopMip(TextureMaxSize, ComputeLog2UvPerPixel(UvDensityPerCm, CmPerPixel), MipBias, Margin, MipCount);
}

float TextureStreamingMath::ComputeUvDensity(const std::vector<FVertex>& Vertices, const std::vector<uint32>& Indices, float Percentile)
{
	struct FSample
	{
		float Density;
		float Weight;
	};
	std::vector<FSample> Samples;
	Samples.reserve(Indices.size() / 3);
	double TotalWeight = 0.0;
	for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
	{
		const uint32 I0 = Indices[Index], I1 = Indices[Index + 1], I2 = Indices[Index + 2];
		if (I0 >= Vertices.size() || I1 >= Vertices.size() || I2 >= Vertices.size())
		{
			continue;
		}
		const FVertex& V0        = Vertices[I0];
		const FVertex& V1        = Vertices[I1];
		const FVertex& V2        = Vertices[I2];
		const float    WorldArea = 0.5f * FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position).Length();
		const FVector2 E1        = V1.UV - V0.UV;
		const FVector2 E2        = V2.UV - V0.UV;
		const float    UvArea    = 0.5f * std::fabs(E1.X * E2.Y - E1.Y * E2.X);
		if (!(WorldArea > 1.0e-8f))
		{
			continue; // 월드에서 퇴화 (픽셀을 덮지 않는다)
		}
		// UV가 모인 삼각형(면적 0)은 밀도 0 — UV 미분이 0이라 하드웨어는 밉 0을 읽는다
		Samples.push_back({ UvArea > 0.0f ? std::sqrt(UvArea / WorldArea) : 0.0f, WorldArea });
		TotalWeight += WorldArea;
	}
	if (Samples.empty() || !(TotalWeight > 0.0))
	{
		return 0.0f;
	}
	std::sort(Samples.begin(), Samples.end(), [](const FSample& A, const FSample& B) { return A.Density < B.Density; });
	const double Target = TotalWeight * std::clamp(static_cast<double>(Percentile), 0.0, 1.0);
	double       Sum    = 0.0;
	for (const FSample& Sample : Samples)
	{
		Sum += Sample.Weight;
		if (Sum >= Target)
		{
			return Sample.Density;
		}
	}
	return Samples.back().Density;
}

float TextureStreamingMath::ComputeGraphTextureUvScale(const FMaterialGraph& Graph, std::string_view ParameterName)
{
	float Scale  = 0.0f;
	bool  bFound = false;
	for (const FMaterialGraphNode& Node : Graph.Nodes)
	{
		if (Node.Type != "TextureSample" || Node.Name != ParameterName)
		{
			continue;
		}
		bFound                         = true;
		const FMaterialGraphInput* Uv = Node.FindInput("UV");
		if (Uv == nullptr)
		{
			Scale = std::max(Scale, 1.0f); // 연결 없음 = 기본 UV0
			continue;
		}
		if (!Uv->IsLink())
		{
			return -1.0f; // 상수 UV (미분 0 → 하드웨어는 밉 0을 읽는다)
		}
		const FMaterialGraphNode* Source = Graph.FindNode(Uv->Node);
		if (Source == nullptr || Source->Type != "TexCoord")
		{
			return -1.0f; // 계산된 UV
		}
		const float Tiling = std::max(std::fabs(Source->Value.X), std::fabs(Source->Value.Y));
		if (!(Tiling > 0.0f))
		{
			return -1.0f;
		}
		Scale = std::max(Scale, Tiling);
	}
	return bFound ? Scale : 1.0f;
}

uint32 TextureStreamingMath::UpdateHysteresis(FHysteresisState& State, uint32 WantedTop, float DeltaSeconds, float DropDelaySeconds)
{
	if (State.HeldTop == InvalidMip || WantedTop <= State.HeldTop)
	{
		State.HeldTop      = WantedTop;
		State.LowerSeconds = 0.0f;
		return State.HeldTop;
	}
	State.LowerSeconds += std::max(DeltaSeconds, 0.0f);
	if (State.LowerSeconds >= DropDelaySeconds)
	{
		State.HeldTop      = WantedTop;
		State.LowerSeconds = 0.0f;
	}
	return State.HeldTop;
}

TextureStreamingMath::FBudgetResult TextureStreamingMath::FitToBudget(const std::vector<FBudgetItem>& Items, uint64 BudgetBytes)
{
	FBudgetResult Result;
	Result.Tops.resize(Items.size());
	for (size_t Index = 0; Index < Items.size(); ++Index)
	{
		const FBudgetItem& Item = Items[Index];
		Result.Tops[Index]      = std::min(Item.WantedTop, Item.TailTop);
		Result.TotalBytes += Item.RangeBytes != nullptr ? Item.RangeBytes[Result.Tops[Index]] : 0;
	}
	if (Result.TotalBytes <= BudgetBytes)
	{
		return Result;
	}

	// (키, 번호) 최소 힙 — 키 = 우선순위 × (줄인 단계 + 1)
	using FKey = std::pair<float, size_t>;
	std::priority_queue<FKey, std::vector<FKey>, std::greater<FKey>> Heap;
	const auto KeyOf = [&](size_t Index) {
		const FBudgetItem& Item    = Items[Index];
		const float        Dropped = static_cast<float>(Result.Tops[Index] - std::min(Item.WantedTop, Item.TailTop));
		return std::max(Item.Priority, 0.0f) * (Dropped + 1.0f);
	};
	for (size_t Index = 0; Index < Items.size(); ++Index)
	{
		if (Items[Index].RangeBytes != nullptr && Result.Tops[Index] < Items[Index].TailTop)
		{
			Heap.push({ KeyOf(Index), Index });
		}
	}
	while (Result.TotalBytes > BudgetBytes && !Heap.empty())
	{
		const size_t Index = Heap.top().second;
		Heap.pop();
		const FBudgetItem& Item = Items[Index];
		uint32&            Top  = Result.Tops[Index];
		Result.TotalBytes -= Item.RangeBytes[Top] - Item.RangeBytes[Top + 1];
		++Top;
		if (Top < Item.TailTop)
		{
			Heap.push({ KeyOf(Index), Index });
		}
	}
	Result.bOverBudget = Result.TotalBytes > BudgetBytes;
	return Result;
}

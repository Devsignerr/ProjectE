#include "Renderer/TextureCompression.h"

#include "Core/Assert.h"
#include "Core/Math/MathUtils.h"

#pragma warning(push, 0)
#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <execution>
#include <mutex>
#include <numeric>

namespace
{
	// sRGB 8비트 → 선형 (LUT)
	const std::array<float, 256>& GetSrgbToLinearTable()
	{
		static const std::array<float, 256> Table = [] {
			std::array<float, 256> Result{};
			for (int Index = 0; Index < 256; ++Index)
			{
				const float C = static_cast<float>(Index) / 255.0f;
				Result[Index] = C <= 0.04045f ? C / 12.92f : std::pow((C + 0.055f) / 1.055f, 2.4f);
			}
			return Result;
		}();
		return Table;
	}

	uint8 LinearToSrgb8(float Linear)
	{
		Linear        = std::clamp(Linear, 0.0f, 1.0f);
		const float C = Linear <= 0.0031308f ? Linear * 12.92f : 1.055f * std::pow(Linear, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8>(std::lround(C * 255.0f));
	}

	uint8 ToUnorm8(float Value)
	{
		return static_cast<uint8>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
	}

	// 2x2 박스 다운샘플 (홀수 크기는 가장자리 클램프)
	FImage Downsample(const FImage& Source, ETextureUsage Usage)
	{
		FImage Result;
		Result.Width  = std::max(1u, Source.Width / 2);
		Result.Height = std::max(1u, Source.Height / 2);
		Result.Pixels.resize(static_cast<size_t>(Result.Width) * Result.Height * FImage::BytesPerPixel);

		const auto& SrgbTable = GetSrgbToLinearTable();
		auto        Fetch     = [&](uint32 X, uint32 Y) {
            X = std::min(X, Source.Width - 1);
            Y = std::min(Y, Source.Height - 1);
            return &Source.Pixels[(static_cast<size_t>(Y) * Source.Width + X) * FImage::BytesPerPixel];
		};

		for (uint32 Y = 0; Y < Result.Height; ++Y)
		{
			for (uint32 X = 0; X < Result.Width; ++X)
			{
				const uint8* Texels[4] = { Fetch(X * 2, Y * 2), Fetch(X * 2 + 1, Y * 2), Fetch(X * 2, Y * 2 + 1), Fetch(X * 2 + 1, Y * 2 + 1) };
				uint8*       Out       = &Result.Pixels[(static_cast<size_t>(Y) * Result.Width + X) * FImage::BytesPerPixel];

				float Sum[4] = {};
				for (const uint8* Texel : Texels)
				{
					for (int Channel = 0; Channel < 4; ++Channel)
					{
						const bool bSrgbChannel = (Usage == ETextureUsage::Color || Usage == ETextureUsage::PixelArt) && Channel < 3;
						const float Value       = bSrgbChannel ? SrgbTable[Texel[Channel]] : static_cast<float>(Texel[Channel]) / 255.0f;
						Sum[Channel] += Usage == ETextureUsage::Normal && Channel < 3 ? Value * 2.0f - 1.0f : Value;
					}
				}
				for (float& Value : Sum)
				{
					Value *= 0.25f;
				}

				if (Usage == ETextureUsage::Normal)
				{
					// 벡터 평균 후 재정규화 (평균하면 길이가 줄어 조명이 어두워지는 것을 방지)
					const float Length = std::sqrt(Sum[0] * Sum[0] + Sum[1] * Sum[1] + Sum[2] * Sum[2]);
					const float Scale  = Length > 1.0e-6f ? 1.0f / Length : 0.0f;
					for (int Channel = 0; Channel < 3; ++Channel)
					{
						Out[Channel] = ToUnorm8(Length > 1.0e-6f ? Sum[Channel] * Scale * 0.5f + 0.5f : (Channel == 2 ? 1.0f : 0.5f));
					}
				}
				else
				{
					for (int Channel = 0; Channel < 3; ++Channel)
					{
						Out[Channel] = (Usage == ETextureUsage::Color || Usage == ETextureUsage::PixelArt) ? LinearToSrgb8(Sum[Channel]) : ToUnorm8(Sum[Channel]);
					}
				}
				Out[3] = ToUnorm8(Sum[3]);
			}
		}
		return Result;
	}

	ETextureFormat SelectFormat(ETextureUsage Usage)
	{
		switch (Usage)
		{
		case ETextureUsage::Normal: return ETextureFormat::BC5;
		case ETextureUsage::Mask:   return ETextureFormat::BC4;
		default:                    return ETextureFormat::BC7;
		}
	}

	void InitEncoders()
	{
		static std::once_flag Once;
		std::call_once(Once, [] {
			bc7enc_compress_block_init();
			rgbcx::init();
		});
	}

	// 4x4 블록의 RGBA 픽셀 추출 (가장자리 클램프 — 밉 끝단 1x1/2x2 대비)
	void GatherBlock(const FImage& Image, uint32 BlockX, uint32 BlockY, uint8 (&OutPixels)[16 * 4])
	{
		for (uint32 Y = 0; Y < 4; ++Y)
		{
			for (uint32 X = 0; X < 4; ++X)
			{
				const uint32 SourceX = std::min(BlockX * 4 + X, Image.Width - 1);
				const uint32 SourceY = std::min(BlockY * 4 + Y, Image.Height - 1);
				std::memcpy(&OutPixels[(Y * 4 + X) * 4], &Image.Pixels[(static_cast<size_t>(SourceY) * Image.Width + SourceX) * 4], 4);
			}
		}
	}

	FTextureMip EncodeMip(const FImage& Image, ETextureFormat Format, ETextureUsage Usage)
	{
		FTextureMip Mip;
		Mip.Width  = Image.Width;
		Mip.Height = Image.Height;
		if (Format == ETextureFormat::RGBA8)
		{
			Mip.Data = Image.Pixels;
			return Mip;
		}

		const uint32 BlocksX    = (Image.Width + 3) / 4;
		const uint32 BlocksY    = (Image.Height + 3) / 4;
		const uint32 BlockBytes = TextureCompression::GetBlockBytes(Format);
		Mip.Data.resize(static_cast<size_t>(BlocksX) * BlocksY * BlockBytes);

		bc7enc_compress_block_params Params;
		bc7enc_compress_block_params_init(&Params);
		if (Usage != ETextureUsage::Color)
		{
			bc7enc_compress_block_params_init_linear_weights(&Params);
		}

		// 블록 행 단위 병렬 인코딩 (인코더는 초기화 후 스레드 안전)
		std::vector<uint32> Rows(BlocksY);
		std::iota(Rows.begin(), Rows.end(), 0u);
		std::for_each(std::execution::par, Rows.begin(), Rows.end(), [&](uint32 BlockY) {
			uint8 Pixels[16 * 4];
			for (uint32 BlockX = 0; BlockX < BlocksX; ++BlockX)
			{
				GatherBlock(Image, BlockX, BlockY, Pixels);
				uint8* Destination = &Mip.Data[(static_cast<size_t>(BlockY) * BlocksX + BlockX) * BlockBytes];
				switch (Format)
				{
				case ETextureFormat::BC7: bc7enc_compress_block(Destination, Pixels, &Params); break;
				case ETextureFormat::BC5: rgbcx::encode_bc5_hq(Destination, Pixels, 0, 1, 4); break;
				case ETextureFormat::BC4: rgbcx::encode_bc4_hq(Destination, Pixels, 4); break;
				default: break;
				}
			}
		});
		return Mip;
	}
}

size_t FCompressedTexture::GetTotalBytes() const
{
	size_t Total = 0;
	for (const FTextureMip& Mip : Mips)
	{
		Total += Mip.Data.size();
	}
	return Total;
}

uint32 TextureCompression::GetBlockBytes(ETextureFormat Format)
{
	switch (Format)
	{
	case ETextureFormat::BC7: return 16;
	case ETextureFormat::BC5: return 16;
	case ETextureFormat::BC4: return 8;
	default:                  return 0;
	}
}

uint32 TextureCompression::GetRowCount(ETextureFormat Format, uint32 Height)
{
	return GetBlockBytes(Format) > 0 ? (Height + 3) / 4 : Height;
}

uint32 TextureCompression::GetRowBytes(ETextureFormat Format, uint32 Width)
{
	const uint32 BlockBytes = GetBlockBytes(Format);
	return BlockBytes > 0 ? (Width + 3) / 4 * BlockBytes : Width * FImage::BytesPerPixel;
}

size_t TextureCompression::GetMipDataSize(ETextureFormat Format, uint32 Width, uint32 Height)
{
	return static_cast<size_t>(GetRowBytes(Format, Width)) * GetRowCount(Format, Height);
}

std::vector<FImage> TextureCompression::GenerateMips(const FImage& Base, ETextureUsage Usage)
{
	E_CHECKF(Base.IsValid(), "GenerateMips: 잘못된 이미지");
	std::vector<FImage> Mips;
	Mips.reserve(32);
	Mips.push_back(Base);
	while (Mips.back().Width > 1 || Mips.back().Height > 1)
	{
		Mips.push_back(Downsample(Mips.back(), Usage));
	}
	return Mips;
}

FCompressedTexture TextureCompression::Compress(const FImage& Base, ETextureUsage Usage)
{
	InitEncoders();

	FCompressedTexture Result;
	if (Usage == ETextureUsage::PixelArt)
	{
		// 무압축 밉 0 하나 (최근접 Load로만 읽는다 — 밉·블록 압축 없음)
		Result.bSRGB  = true;
		Result.Format = ETextureFormat::RGBA8;
		Result.Mips.push_back(EncodeMip(Base, ETextureFormat::RGBA8, Usage));
		return Result;
	}
	Result.bSRGB  = Usage == ETextureUsage::Color;
	Result.Format = (Base.Width % 4 == 0 && Base.Height % 4 == 0) ? SelectFormat(Usage) : ETextureFormat::RGBA8;

	const std::vector<FImage> Images = GenerateMips(Base, Usage);
	Result.Mips.reserve(Images.size());
	for (const FImage& Image : Images)
	{
		Result.Mips.push_back(EncodeMip(Image, Result.Format, Usage));
	}
	return Result;
}

bool TextureCompression::Decompress(const FCompressedTexture& Texture, uint32 MipIndex, FImage& OutImage)
{
	OutImage = FImage{};
	if (MipIndex >= Texture.Mips.size())
	{
		return false;
	}
	const FTextureMip& Mip = Texture.Mips[MipIndex];
	if (Mip.Data.size() != GetMipDataSize(Texture.Format, Mip.Width, Mip.Height))
	{
		return false;
	}

	OutImage.Width  = Mip.Width;
	OutImage.Height = Mip.Height;
	if (Texture.Format == ETextureFormat::RGBA8)
	{
		OutImage.Pixels = Mip.Data;
		return true;
	}
	OutImage.Pixels.resize(static_cast<size_t>(Mip.Width) * Mip.Height * FImage::BytesPerPixel);

	const uint32 BlocksX    = (Mip.Width + 3) / 4;
	const uint32 BlocksY    = (Mip.Height + 3) / 4;
	const uint32 BlockBytes = GetBlockBytes(Texture.Format);
	for (uint32 BlockY = 0; BlockY < BlocksY; ++BlockY)
	{
		for (uint32 BlockX = 0; BlockX < BlocksX; ++BlockX)
		{
			const uint8* Block = &Mip.Data[(static_cast<size_t>(BlockY) * BlocksX + BlockX) * BlockBytes];
			uint8        Pixels[16 * 4];
			switch (Texture.Format)
			{
			case ETextureFormat::BC7:
				bc7decomp::unpack_bc7(Block, reinterpret_cast<bc7decomp::color_rgba*>(Pixels));
				break;
			case ETextureFormat::BC5:
				for (int Index = 0; Index < 16; ++Index)
				{
					Pixels[Index * 4 + 2] = 0;
					Pixels[Index * 4 + 3] = 255;
				}
				rgbcx::unpack_bc5(Block, Pixels, 0, 1, 4);
				break;
			case ETextureFormat::BC4:
				for (int Index = 0; Index < 16; ++Index)
				{
					Pixels[Index * 4 + 1] = 0;
					Pixels[Index * 4 + 2] = 0;
					Pixels[Index * 4 + 3] = 255;
				}
				rgbcx::unpack_bc4(Block, Pixels, 4);
				break;
			default:
				return false;
			}

			for (uint32 Y = 0; Y < 4; ++Y)
			{
				for (uint32 X = 0; X < 4; ++X)
				{
					const uint32 TargetX = BlockX * 4 + X;
					const uint32 TargetY = BlockY * 4 + Y;
					if (TargetX < Mip.Width && TargetY < Mip.Height)
					{
						std::memcpy(&OutImage.Pixels[(static_cast<size_t>(TargetY) * Mip.Width + TargetX) * 4], &Pixels[(Y * 4 + X) * 4], 4);
					}
				}
			}
		}
	}
	return true;
}

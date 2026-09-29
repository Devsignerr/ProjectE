#include "Core/Testing/TestFramework.h"
#include "RHI/TextureUtils.h"
#include "Renderer/TextureCompression.h"

#include <cmath>

namespace
{
	FImage MakeImage(uint32 Width, uint32 Height, auto&& PixelFunc)
	{
		FImage Image;
		Image.Width  = Width;
		Image.Height = Height;
		Image.Pixels.resize(static_cast<size_t>(Width) * Height * 4);
		for (uint32 Y = 0; Y < Height; ++Y)
		{
			for (uint32 X = 0; X < Width; ++X)
			{
				PixelFunc(X, Y, &Image.Pixels[(static_cast<size_t>(Y) * Width + X) * 4]);
			}
		}
		return Image;
	}

	// 채널별 최대 절대 오차
	int32 MaxError(const FImage& A, const FImage& B, int32 ChannelCount)
	{
		int32 Error = 0;
		for (size_t Index = 0; Index < A.Pixels.size(); Index += 4)
		{
			for (int32 Channel = 0; Channel < ChannelCount; ++Channel)
			{
				Error = std::max(Error, std::abs(static_cast<int32>(A.Pixels[Index + Channel]) - static_cast<int32>(B.Pixels[Index + Channel])));
			}
		}
		return Error;
	}

	// 평균 제곱 오차 기반 PSNR (RGB)
	double Psnr(const FImage& A, const FImage& B)
	{
		double SumSquared = 0.0;
		size_t Count      = 0;
		for (size_t Index = 0; Index < A.Pixels.size(); Index += 4)
		{
			for (int Channel = 0; Channel < 3; ++Channel)
			{
				const double Diff = static_cast<double>(A.Pixels[Index + Channel]) - static_cast<double>(B.Pixels[Index + Channel]);
				SumSquared += Diff * Diff;
				++Count;
			}
		}
		const double Mse = SumSquared / static_cast<double>(Count);
		return Mse <= 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / Mse);
	}
}

E_TEST(TextureCompression_MipDataSizes)
{
	E_EXPECT_EQ(TextureCompression::GetMipDataSize(ETextureFormat::BC7, 256, 256), static_cast<size_t>(64 * 64 * 16));
	E_EXPECT_EQ(TextureCompression::GetMipDataSize(ETextureFormat::BC5, 256, 256), static_cast<size_t>(64 * 64 * 16));
	E_EXPECT_EQ(TextureCompression::GetMipDataSize(ETextureFormat::BC4, 256, 256), static_cast<size_t>(64 * 64 * 8));
	E_EXPECT_EQ(TextureCompression::GetMipDataSize(ETextureFormat::BC7, 2, 1), static_cast<size_t>(16)); // 작은 밉도 블록 1개
	E_EXPECT_EQ(TextureCompression::GetMipDataSize(ETextureFormat::RGBA8, 3, 5), static_cast<size_t>(3 * 5 * 4));
	E_EXPECT_EQ(TextureCompression::GetRowCount(ETextureFormat::BC7, 6), 2u);
	E_EXPECT_EQ(TextureCompression::GetRowBytes(ETextureFormat::BC4, 9), 24u);
}

E_TEST(TextureCompression_MipChainLength)
{
	const FImage Base = MakeImage(256, 64, [](uint32, uint32, uint8* P) { P[0] = P[1] = P[2] = P[3] = 255; });
	const auto   Mips = TextureCompression::GenerateMips(Base, ETextureUsage::Linear);
	E_EXPECT_EQ(static_cast<uint32>(Mips.size()), CalculateMipCount(256, 64));
	E_EXPECT_EQ(Mips.back().Width, 1u);
	E_EXPECT_EQ(Mips.back().Height, 1u);
	E_EXPECT_EQ(Mips[1].Width, 128u);
	E_EXPECT_EQ(Mips[1].Height, 32u);
}

E_TEST(TextureCompression_ColorMipsAverageInLinearSpace)
{
	// 흑백 체커 → 선형 평균 0.5 → sRGB 약 188 (sRGB 값 그대로 평균하면 128이 되어 어두워짐)
	const FImage Checker = MakeImage(2, 2, [](uint32 X, uint32 Y, uint8* P) {
		const uint8 Value = (X + Y) % 2 == 0 ? 255 : 0;
		P[0] = P[1] = P[2] = Value;
		P[3] = 255;
	});
	const auto ColorMips  = TextureCompression::GenerateMips(Checker, ETextureUsage::Color);
	const auto LinearMips = TextureCompression::GenerateMips(Checker, ETextureUsage::Linear);
	E_EXPECT_NEAR(static_cast<float>(ColorMips[1].Pixels[0]), 188.0f, 1.0f);
	E_EXPECT_NEAR(static_cast<float>(LinearMips[1].Pixels[0]), 128.0f, 1.0f);
}

E_TEST(TextureCompression_NormalMipsRenormalize)
{
	// +X와 +Y로 기운 노멀 절반씩 → 평균 후 단위 길이
	const FImage Normals = MakeImage(2, 2, [](uint32 X, uint32, uint8* P) {
		const float Nx = X == 0 ? 0.6f : 0.0f;
		const float Ny = X == 0 ? 0.0f : 0.6f;
		P[0] = static_cast<uint8>(std::lround((Nx * 0.5f + 0.5f) * 255.0f));
		P[1] = static_cast<uint8>(std::lround((Ny * 0.5f + 0.5f) * 255.0f));
		P[2] = static_cast<uint8>(std::lround((0.8f * 0.5f + 0.5f) * 255.0f));
		P[3] = 255;
	});
	const auto   Mips = TextureCompression::GenerateMips(Normals, ETextureUsage::Normal);
	const uint8* P    = Mips[1].Pixels.data();
	const float  Nx   = P[0] / 255.0f * 2.0f - 1.0f;
	const float  Ny   = P[1] / 255.0f * 2.0f - 1.0f;
	const float  Nz   = P[2] / 255.0f * 2.0f - 1.0f;
	E_EXPECT_NEAR(std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz), 1.0f, 0.02f);
	E_EXPECT_NEAR(Nx, Ny, 0.01f);
}

E_TEST(TextureCompression_Bc7RoundTrip)
{
	const FImage Gradient = MakeImage(64, 64, [](uint32 X, uint32 Y, uint8* P) {
		P[0] = static_cast<uint8>(X * 4);
		P[1] = static_cast<uint8>(Y * 4);
		P[2] = static_cast<uint8>(255 - X * 2);
		P[3] = X < 32 ? 255 : 128; // 알파 보존 확인
	});
	const FCompressedTexture Texture = TextureCompression::Compress(Gradient, ETextureUsage::Color);
	E_EXPECT_TRUE(Texture.Format == ETextureFormat::BC7);
	E_EXPECT_TRUE(Texture.bSRGB);
	E_EXPECT_EQ(static_cast<uint32>(Texture.Mips.size()), CalculateMipCount(64, 64));
	E_EXPECT_EQ(Texture.Mips[0].Data.size(), static_cast<size_t>(16 * 16 * 16));

	FImage Decoded;
	E_EXPECT_TRUE(TextureCompression::Decompress(Texture, 0, Decoded));
	E_EXPECT_TRUE(Psnr(Gradient, Decoded) > 38.0);
	E_EXPECT_NEAR(static_cast<float>(Decoded.Pixels[3]), 255.0f, 2.0f);
	E_EXPECT_NEAR(static_cast<float>(Decoded.Pixels[(63 * 4) + 3]), 128.0f, 4.0f);

	// 가장 작은 밉도 디코딩 가능
	E_EXPECT_TRUE(TextureCompression::Decompress(Texture, static_cast<uint32>(Texture.Mips.size() - 1), Decoded));
	E_EXPECT_EQ(Decoded.Width, 1u);
}

E_TEST(TextureCompression_Bc5NormalRoundTrip)
{
	const FImage Normals = MakeImage(32, 32, [](uint32 X, uint32 Y, uint8* P) {
		const float Nx = (static_cast<float>(X) / 31.0f - 0.5f) * 0.8f;
		const float Ny = (static_cast<float>(Y) / 31.0f - 0.5f) * 0.8f;
		const float Nz = std::sqrt(1.0f - Nx * Nx - Ny * Ny);
		P[0] = static_cast<uint8>(std::lround((Nx * 0.5f + 0.5f) * 255.0f));
		P[1] = static_cast<uint8>(std::lround((Ny * 0.5f + 0.5f) * 255.0f));
		P[2] = static_cast<uint8>(std::lround((Nz * 0.5f + 0.5f) * 255.0f));
		P[3] = 255;
	});
	const FCompressedTexture Texture = TextureCompression::Compress(Normals, ETextureUsage::Normal);
	E_EXPECT_TRUE(Texture.Format == ETextureFormat::BC5);
	E_EXPECT_FALSE(Texture.bSRGB);

	FImage Decoded;
	E_EXPECT_TRUE(TextureCompression::Decompress(Texture, 0, Decoded));
	E_EXPECT_TRUE(MaxError(Normals, Decoded, 2) <= 3); // XY만 저장
	E_EXPECT_EQ(Decoded.Pixels[2], static_cast<uint8>(0));
}

E_TEST(TextureCompression_Bc4MaskRoundTrip)
{
	const FImage Mask = MakeImage(16, 16, [](uint32 X, uint32 Y, uint8* P) {
		P[0] = static_cast<uint8>(X * 16 + Y); // 최대 255, 랩어라운드 없음
		P[1] = P[2] = 0;
		P[3] = 255;
	});
	const FCompressedTexture Texture = TextureCompression::Compress(Mask, ETextureUsage::Mask);
	E_EXPECT_TRUE(Texture.Format == ETextureFormat::BC4);
	E_EXPECT_EQ(Texture.Mips[0].Data.size(), static_cast<size_t>(4 * 4 * 8));

	FImage Decoded;
	E_EXPECT_TRUE(TextureCompression::Decompress(Texture, 0, Decoded));
	E_EXPECT_TRUE(MaxError(Mask, Decoded, 1) <= 8);
}

E_TEST(TextureCompression_NonBlockSizeFallsBackToRgba8)
{
	const FImage Odd = MakeImage(6, 3, [](uint32 X, uint32, uint8* P) { P[0] = P[1] = P[2] = static_cast<uint8>(X * 40); P[3] = 255; });
	const FCompressedTexture Texture = TextureCompression::Compress(Odd, ETextureUsage::Color);
	E_EXPECT_TRUE(Texture.Format == ETextureFormat::RGBA8);
	E_EXPECT_EQ(static_cast<uint32>(Texture.Mips.size()), CalculateMipCount(6, 3));

	FImage Decoded;
	E_EXPECT_TRUE(TextureCompression::Decompress(Texture, 0, Decoded));
	E_EXPECT_EQ(MaxError(Odd, Decoded, 4), 0);
}

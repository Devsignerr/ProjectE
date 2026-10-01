#include "Renderer/Image.h"

#include "Core/CoreMinimal.h"
#include "Core/FileSystem.h"
#include "Core/Math/MathUtils.h"
#include "Core/StringConv.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// stb_image 구현은 이 파일에서만 인스턴스화. 서드파티 코드의 경고는 무시한다.
#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h> // HDR 지원 (Phase 33-7: 환경맵 stbi_loadf)
#pragma warning(pop)

E_DECLARE_LOG_CATEGORY(LogRenderer)
E_DEFINE_LOG_CATEGORY(LogRenderer, Log)

namespace
{
	bool CopyDecoded(stbi_uc* Data, int Width, int Height, FImage& OutImage)
	{
		OutImage.Width  = static_cast<uint32>(Width);
		OutImage.Height = static_cast<uint32>(Height);
		OutImage.Pixels.resize(static_cast<size_t>(OutImage.Width) * OutImage.Height * FImage::BytesPerPixel);
		std::memcpy(OutImage.Pixels.data(), Data, OutImage.Pixels.size());
		stbi_image_free(Data);
		return true;
	}
} // namespace

FImage FImage::MakeSolidColor(uint32 InWidth, uint32 InHeight, uint8 R, uint8 G, uint8 B, uint8 A)
{
	FImage Image;
	Image.Width  = InWidth;
	Image.Height = InHeight;
	Image.Pixels.resize(static_cast<size_t>(InWidth) * InHeight * BytesPerPixel);
	for (size_t Index = 0; Index < Image.Pixels.size(); Index += BytesPerPixel)
	{
		Image.Pixels[Index + 0] = R;
		Image.Pixels[Index + 1] = G;
		Image.Pixels[Index + 2] = B;
		Image.Pixels[Index + 3] = A;
	}
	return Image;
}

bool FImageLoader::LoadFromFile(const std::filesystem::path& Path, FImage& OutImage)
{
	OutImage = FImage{};

	// pak → 디스크 (FFileSystem). 파일 전체를 읽어 메모리에서 디코딩한다
	std::vector<uint8> Bytes;
	if (!FFileSystem::ReadFile(Path, Bytes))
	{
		E_LOG(LogRenderer, Error, "이미지 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	const std::string Name = FStringConv::ToUtf8(Path.filename().wstring());
	return LoadFromMemory(Bytes.data(), Bytes.size(), OutImage, Name.c_str());
}

bool FImageLoader::LoadFromMemory(const uint8* Data, size_t SizeInBytes, FImage& OutImage, const char* DebugName)
{
	OutImage = FImage{};

	int      Width    = 0;
	int      Height   = 0;
	int      Channels = 0;
	stbi_uc* Decoded  = stbi_load_from_memory(Data, static_cast<int>(SizeInBytes), &Width, &Height, &Channels, STBI_rgb_alpha);
	if (Decoded == nullptr)
	{
		E_LOG(LogRenderer, Error, "이미지 디코딩 실패: {} ({})", DebugName, stbi_failure_reason());
		return false;
	}

	CopyDecoded(Decoded, Width, Height, OutImage);
	E_LOG(LogRenderer, Log, "이미지 로드: {} ({}x{}, 원본 채널 {})", DebugName, OutImage.Width, OutImage.Height, Channels);
	return true;
}

uint16 FHalfFloat::FromFloat(float Value)
{
	uint32 Bits;
	std::memcpy(&Bits, &Value, sizeof(Bits));
	const uint32 Sign     = (Bits >> 16) & 0x8000u;
	const int32  Exponent = static_cast<int32>((Bits >> 23) & 0xFFu);
	uint32       Mantissa = Bits & 0x7FFFFFu;
	if (Exponent == 0xFF)
	{
		return static_cast<uint16>(Sign | 0x7C00u | (Mantissa != 0 ? 0x200u : 0u)); // 무한대 / NaN
	}
	const int32 HalfExponent = Exponent - 127 + 15;
	if (HalfExponent >= 0x1F)
	{
		return static_cast<uint16>(Sign | 0x7C00u); // 범위 밖 → 무한대
	}
	if (HalfExponent <= 0)
	{
		if (HalfExponent < -10)
		{
			return static_cast<uint16>(Sign); // 0으로
		}
		// 비정규 수: 숨은 1을 붙이고 밀어낸 뒤 반올림
		Mantissa |= 0x800000u;
		const uint32 Shift   = static_cast<uint32>(14 - HalfExponent);
		uint32       Half    = Mantissa >> Shift;
		const uint32 Remain  = Mantissa & ((1u << Shift) - 1u);
		const uint32 Halfway = 1u << (Shift - 1u);
		if (Remain > Halfway || (Remain == Halfway && (Half & 1u) != 0))
		{
			++Half;
		}
		return static_cast<uint16>(Sign | Half);
	}
	uint32       Half   = Sign | (static_cast<uint32>(HalfExponent) << 10) | (Mantissa >> 13);
	const uint32 Remain = Mantissa & 0x1FFFu;
	if (Remain > 0x1000u || (Remain == 0x1000u && (Half & 1u) != 0))
	{
		++Half; // 넘치면 지수로 올라가 올바른 값(또는 무한대)이 된다
	}
	return static_cast<uint16>(Half);
}

float FHalfFloat::ToFloat(uint16 Value)
{
	const uint32 Sign     = (static_cast<uint32>(Value) & 0x8000u) << 16;
	uint32       Exponent = (Value >> 10) & 0x1Fu;
	uint32       Mantissa = Value & 0x3FFu;
	uint32       Bits;
	if (Exponent == 0)
	{
		if (Mantissa == 0)
		{
			Bits = Sign;
		}
		else
		{
			// 비정규 → 정규화
			Exponent = 127 - 15 + 1;
			while ((Mantissa & 0x400u) == 0)
			{
				Mantissa <<= 1;
				--Exponent;
			}
			Mantissa &= 0x3FFu;
			Bits = Sign | (Exponent << 23) | (Mantissa << 13);
		}
	}
	else if (Exponent == 0x1F)
	{
		Bits = Sign | 0x7F800000u | (Mantissa << 13);
	}
	else
	{
		Bits = Sign | ((Exponent - 15 + 127) << 23) | (Mantissa << 13);
	}
	float Result;
	std::memcpy(&Result, &Bits, sizeof(Result));
	return Result;
}

bool FImageLoader::LoadEnvironmentFromFile(const std::filesystem::path& Path, FEnvironmentImage& OutImage, uint32 MaxWidth)
{
	OutImage = FEnvironmentImage{};
	std::vector<uint8> Bytes;
	if (!FFileSystem::ReadFile(Path, Bytes))
	{
		E_LOG(LogRenderer, Error, "환경맵 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	int    Width    = 0;
	int    Height   = 0;
	int    Channels = 0;
	float* Decoded  = stbi_loadf_from_memory(Bytes.data(), static_cast<int>(Bytes.size()), &Width, &Height, &Channels, 3);
	if (Decoded == nullptr || Width <= 0 || Height <= 0)
	{
		E_LOG(LogRenderer, Error, "환경맵 디코딩 실패: {} ({})", FStringConv::ToUtf8(Path.filename().wstring()), stbi_failure_reason());
		return false;
	}
	// 너무 크면 정수 배로 상자 필터 축소 (메모리/큐브 변환 시간)
	uint32 Factor = 1;
	while (static_cast<uint32>(Width) / Factor > FMath::Max(MaxWidth, 1u))
	{
		Factor *= 2;
	}
	OutImage.Width  = FMath::Max(1u, static_cast<uint32>(Width) / Factor);
	OutImage.Height = FMath::Max(1u, static_cast<uint32>(Height) / Factor);
	OutImage.Pixels.resize(static_cast<size_t>(OutImage.Width) * OutImage.Height * 4);
	for (uint32 Y = 0; Y < OutImage.Height; ++Y)
	{
		for (uint32 X = 0; X < OutImage.Width; ++X)
		{
			float Sum[3] = {};
			for (uint32 SY = 0; SY < Factor; ++SY)
			{
				for (uint32 SX = 0; SX < Factor; ++SX)
				{
					const size_t Source = (static_cast<size_t>(Y * Factor + SY) * static_cast<size_t>(Width) + (X * Factor + SX)) * 3;
					for (uint32 C = 0; C < 3; ++C)
					{
						Sum[C] += FMath::Max(Decoded[Source + C], 0.0f);
					}
				}
			}
			const float  Scale = 1.0f / static_cast<float>(Factor * Factor);
			const size_t Dest  = (static_cast<size_t>(Y) * OutImage.Width + X) * 4;
			for (uint32 C = 0; C < 3; ++C)
			{
				OutImage.Pixels[Dest + C] = FHalfFloat::FromFloat(FMath::Min(Sum[C] * Scale, 65000.0f));
			}
			OutImage.Pixels[Dest + 3] = FHalfFloat::FromFloat(1.0f);
		}
	}
	stbi_image_free(Decoded);
	E_LOG(LogRenderer, Log, "환경맵 로드: {} ({}x{} → {}x{})", FStringConv::ToUtf8(Path.filename().wstring()), Width, Height, OutImage.Width, OutImage.Height);
	return true;
}

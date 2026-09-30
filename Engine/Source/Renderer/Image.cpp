#include "Renderer/Image.h"

#include "Core/CoreMinimal.h"
#include "Core/FileSystem.h"
#include "Core/StringConv.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// stb_image 구현은 이 파일에서만 인스턴스화. 서드파티 코드의 경고는 무시한다.
#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb_image.h>
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

#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <vector>

// CPU 측 이미지 (항상 RGBA8, 상단 행부터)
struct FImage
{
	uint32             Width  = 0;
	uint32             Height = 0;
	std::vector<uint8> Pixels; // Width * Height * 4

	static constexpr uint32 BytesPerPixel = 4;

	bool   IsValid() const { return Width > 0 && Height > 0 && Pixels.size() == static_cast<size_t>(Width) * Height * BytesPerPixel; }
	uint32 GetRowPitch() const { return Width * BytesPerPixel; }

	// 단색 이미지 (기본 텍스처용)
	static FImage MakeSolidColor(uint32 InWidth, uint32 InHeight, uint8 R, uint8 G, uint8 B, uint8 A = 255);
};

// 이미지 로더 (stb_image: PNG/JPEG/BMP/TGA/PSD/GIF 등)
struct FImageLoader
{
	// 실패 시 false 반환, OutImage는 비워진다
	static bool LoadFromFile(const std::filesystem::path& Path, FImage& OutImage);
	static bool LoadFromMemory(const uint8* Data, size_t SizeInBytes, FImage& OutImage, const char* DebugName = "memory");
};

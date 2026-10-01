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

// 16비트 부동소수점 (IEEE 754 half) 변환 — RGBA16F 업로드/쿠킹용 (반올림: 가장 가까운 값, 범위 밖은 무한대, NaN 유지)
struct FHalfFloat
{
	static uint16 FromFloat(float Value);
	static float  ToFloat(uint16 Value);
};

// HDR 환경 이미지 (등장방형, 상단 행부터, RGBA16F). 하늘/IBL 소스 (.hdr → FAssetCache .eenv)
struct FEnvironmentImage
{
	uint32              Width  = 0;
	uint32              Height = 0;
	std::vector<uint16> Pixels; // Width * Height * 4 (half)

	bool IsValid() const { return Width > 0 && Height > 0 && Pixels.size() == static_cast<size_t>(Width) * Height * 4; }
};

// 이미지 로더 (stb_image: PNG/JPEG/BMP/TGA/PSD/GIF/HDR 등)
struct FImageLoader
{
	// 실패 시 false 반환, OutImage는 비워진다
	static bool LoadFromFile(const std::filesystem::path& Path, FImage& OutImage);
	static bool LoadFromMemory(const uint8* Data, size_t SizeInBytes, FImage& OutImage, const char* DebugName = "memory");
	// HDR(.hdr 등 stbi_loadf 지원 형식) → 선형 RGBA16F. MaxWidth보다 넓으면 상자 필터로 줄인다 (높이도 같은 비율)
	static bool LoadEnvironmentFromFile(const std::filesystem::path& Path, FEnvironmentImage& OutImage, uint32 MaxWidth = 2048);
};

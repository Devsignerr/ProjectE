#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <vector>

// 높이맵 이미지 읽기 (지형 가져오기): PNG/TGA 등 → 16비트 회색조. 8비트 이미지는 0~65535로 늘린다 (stb_image, Image.cpp가 구현).
// RAW16은 TerrainIO::ReadRaw16, 쓰기(PNG16/RAW16)는 TerrainIO
struct FTerrainHeightmapImage
{
	static bool Load(const std::filesystem::path& Path, std::vector<uint16>& OutPixels, uint32& OutWidth, uint32& OutHeight);
};

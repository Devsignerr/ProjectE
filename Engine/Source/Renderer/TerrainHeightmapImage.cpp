#include "Renderer/TerrainHeightmapImage.h"

#include "Core/FileSystem.h"

// stb_image 구현은 Image.cpp에 있다 (같은 엔진 DLL). 여기서는 선언만 포함한다
#pragma warning(push, 0)
#include <stb_image.h>
#pragma warning(pop)

bool FTerrainHeightmapImage::Load(const std::filesystem::path& Path, std::vector<uint16>& OutPixels, uint32& OutWidth, uint32& OutHeight)
{
	std::vector<uint8> Bytes;
	if (!FFileSystem::ReadFile(Path, Bytes) || Bytes.empty())
	{
		return false;
	}
	int     Width    = 0;
	int     Height   = 0;
	int     Channels = 0;
	stbi_us* Pixels  = stbi_load_16_from_memory(Bytes.data(), static_cast<int>(Bytes.size()), &Width, &Height, &Channels, 1); // 회색조 1채널
	if (Pixels == nullptr || Width < 2 || Height < 2)
	{
		stbi_image_free(Pixels);
		return false;
	}
	OutWidth  = static_cast<uint32>(Width);
	OutHeight = static_cast<uint32>(Height);
	OutPixels.assign(Pixels, Pixels + static_cast<size_t>(Width) * Height);
	stbi_image_free(Pixels);
	return true;
}

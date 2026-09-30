#include "Core/Serialization/BinaryArchive.h"

#include "Core/FileSystem.h"

#include <fstream>

bool FBinaryWriter::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);

	// 쓰는 도중 중단되어도 기존 파일이 깨지지 않도록 임시 파일에 쓴 뒤 교체
	std::filesystem::path TempPath = Path;
	TempPath += L".tmp";
	{
		std::ofstream File(TempPath, std::ios::binary | std::ios::trunc);
		if (!File)
		{
			return false;
		}
		File.write(reinterpret_cast<const char*>(Buffer.data()), static_cast<std::streamsize>(Buffer.size()));
		if (!File)
		{
			return false;
		}
	}
	std::filesystem::rename(TempPath, Path, ErrorCode);
	if (ErrorCode)
	{
		std::filesystem::remove(TempPath, ErrorCode);
		return false;
	}
	return true;
}

bool ReadFileBytes(const std::filesystem::path& Path, std::vector<uint8>& OutBytes)
{
	return FFileSystem::ReadFile(Path, OutBytes); // pak → 디스크
}

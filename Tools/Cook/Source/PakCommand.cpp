#include "PakCommand.h"

#include "Core/CoreMinimal.h"
#include "Core/FileSystem.h"
#include "Core/StringConv.h"

#include <algorithm>

E_DEFINE_LOG_CATEGORY(LogPak, Log)

int MakePak(const std::filesystem::path& PakFile, const std::filesystem::path& Root, const std::vector<std::filesystem::path>& Directories)
{
	std::vector<std::pair<std::string, std::filesystem::path>> Files;
	uint64 TotalBytes = 0;
	for (const std::filesystem::path& Relative : Directories)
	{
		const std::filesystem::path Directory = Root / Relative;
		std::error_code             ErrorCode;
		if (!std::filesystem::is_directory(Directory, ErrorCode))
		{
			E_LOG(LogPak, Warning, "pak에 넣을 폴더가 없습니다 (건너뜀): {}", FStringConv::ToUtf8(Directory.wstring()));
			continue;
		}
		for (const std::filesystem::directory_entry& Entry : std::filesystem::recursive_directory_iterator(Directory, ErrorCode))
		{
			if (!Entry.is_regular_file(ErrorCode))
			{
				continue;
			}
			const std::string Key = FFileSystem::MakeKey(Entry.path(), Root);
			if (Key.empty())
			{
				E_LOG(LogPak, Error, "pak 루트 밖의 파일입니다: {}", FStringConv::ToUtf8(Entry.path().wstring()));
				return 1;
			}
			TotalBytes += Entry.file_size(ErrorCode);
			Files.emplace_back(Key, Entry.path());
		}
	}
	// 같은 입력이면 같은 pak (Steam 차등 업로드가 작아진다)
	std::sort(Files.begin(), Files.end(), [](const auto& A, const auto& B) { return A.first < B.first; });

	std::string Error;
	if (!FPakWriter::Write(PakFile, Files, Error))
	{
		E_LOG(LogPak, Error, "{}", Error);
		return 1;
	}
	E_LOG(LogPak, Display, "pak 생성: {} (파일 {}개, {:.1f} MB)", FStringConv::ToUtf8(PakFile.wstring()), Files.size(), TotalBytes / (1024.0 * 1024.0));
	return 0;
}

#include "Core/FileSystem.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	void WriteText(const fs::path& Path, const std::string& Text)
	{
		std::error_code ErrorCode;
		fs::create_directories(Path.parent_path(), ErrorCode);
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
	}
} // namespace

E_TEST(FileSystem_MakeKey)
{
	const fs::path Root = FTestRegistry::GetTempDirectory() / L"PakRoot";
	E_EXPECT_TRUE(FFileSystem::MakeKey(Root / L"Game" / L"Content" / L"Scene.escene", Root) == "game/content/scene.escene");
	// 대소문자/구분자/.. 정규화
	E_EXPECT_TRUE(FFileSystem::MakeKey(Root / L"GAME" / L"x" / L".." / L"Content\\A.lua", Root) == "game/content/a.lua");
	E_EXPECT_TRUE(FFileSystem::MakeKey(Root / L"한글" / L"파일.json", Root) == "한글/파일.json");
	// 루트 밖 / 루트 자체
	E_EXPECT_TRUE(FFileSystem::MakeKey(Root.parent_path() / L"Other.txt", Root).empty());
	E_EXPECT_TRUE(FFileSystem::MakeKey(Root, Root).empty());
}

E_TEST(FileSystem_PakMountAndRead)
{
	const fs::path Base   = FTestRegistry::GetTempDirectory() / L"ProjectE_PakTest";
	const fs::path Source = Base / L"Source";
	const fs::path Mount  = Base / L"Mounted"; // 디스크에는 없는 루트 (pak에서만 읽힌다)
	std::error_code ErrorCode;
	fs::remove_all(Base, ErrorCode);
	WriteText(Source / L"Game" / L"Content" / L"Scene.escene", "{ \"Entities\": [] }");
	WriteText(Source / L"Game" / L"Content" / L"빈 파일.txt", "");
	std::vector<uint8> Binary(70000);
	for (size_t Index = 0; Index < Binary.size(); ++Index)
	{
		Binary[Index] = static_cast<uint8>(Index * 31);
	}
	{
		std::ofstream File(Source / L"Game" / L"Cooked" / L"Model.emodel", std::ios::binary);
		fs::create_directories(Source / L"Game" / L"Cooked", ErrorCode);
	}
	{
		std::ofstream File(Source / L"Game" / L"Cooked" / L"Model.emodel", std::ios::binary | std::ios::trunc);
		File.write(reinterpret_cast<const char*>(Binary.data()), static_cast<std::streamsize>(Binary.size()));
	}

	std::vector<std::pair<std::string, fs::path>> Files;
	for (const fs::directory_entry& Entry : fs::recursive_directory_iterator(Source))
	{
		if (Entry.is_regular_file())
		{
			Files.emplace_back(FFileSystem::MakeKey(Entry.path(), Source), Entry.path());
		}
	}
	const fs::path Pak = Base / L"Game.epak";
	std::string    Error;
	E_EXPECT_TRUE(FPakWriter::Write(Pak, Files, Error));
	// 키 중복은 실패
	std::vector<std::pair<std::string, fs::path>> Duplicate = { Files[0], Files[0] };
	E_EXPECT_FALSE(FPakWriter::Write(Base / L"Dup.epak", Duplicate, Error));

	E_EXPECT_TRUE(FFileSystem::Mount(Pak, Mount));
	E_EXPECT_EQ(FFileSystem::GetMountedFileCount(), static_cast<size_t>(3));

	const fs::path SceneInPak = Mount / L"game" / L"CONTENT" / L"Scene.escene"; // 대소문자 무시
	E_EXPECT_TRUE(FFileSystem::Exists(SceneInPak));
	E_EXPECT_TRUE(FFileSystem::IsInPak(SceneInPak));
	std::string Text;
	E_EXPECT_TRUE(FFileSystem::ReadTextFile(SceneInPak, Text));
	E_EXPECT_TRUE(Text == "{ \"Entities\": [] }");
	E_EXPECT_TRUE(FFileSystem::ReadTextFile(Mount / L"Game" / L"Content" / L"빈 파일.txt", Text) && Text.empty());

	// 바이너리 + ReadFileBytes(쿠킹 로더 경로)도 pak을 본다
	std::vector<uint8> Read;
	E_EXPECT_TRUE(ReadFileBytes(Mount / L"Game" / L"Cooked" / L"Model.emodel", Read));
	E_EXPECT_TRUE(Read == Binary);
	// pak 항목 시각 = pak 파일 시각
	E_EXPECT_TRUE(FFileSystem::GetLastWriteTime(SceneInPak) == fs::last_write_time(Pak));

	// 없는 항목은 디스크로 (디스크에도 없으면 실패), 디스크 파일은 그대로 읽힌다
	E_EXPECT_FALSE(FFileSystem::Exists(Mount / L"Game" / L"None.txt"));
	E_EXPECT_FALSE(FFileSystem::ReadTextFile(Mount / L"Game" / L"None.txt", Text));
	E_EXPECT_TRUE(FFileSystem::ReadTextFile(Source / L"Game" / L"Content" / L"Scene.escene", Text));
	E_EXPECT_FALSE(FFileSystem::IsInPak(Source / L"Game" / L"Content" / L"Scene.escene"));
	E_EXPECT_FALSE(FFileSystem::GetLastWriteTime(Mount / L"Game" / L"None.txt").has_value());
	FFileSystem::UnmountAll();
	E_EXPECT_FALSE(FFileSystem::Exists(SceneInPak));

	// 데이터가 손상되면 해시 불일치로 읽기 실패
	{
		std::fstream File(Pak, std::ios::binary | std::ios::in | std::ios::out);
		File.seekp(24 + 5);
		File.put('#');
	}
	E_EXPECT_TRUE(FFileSystem::Mount(Pak, Mount));
	const bool bCorruptRead = FFileSystem::ReadFile(Mount / L"Game" / L"Cooked" / L"Model.emodel", Read) &&
	                          FFileSystem::ReadFile(Mount / L"Game" / L"Content" / L"Scene.escene", Read);
	E_EXPECT_FALSE(bCorruptRead);
	FFileSystem::UnmountAll();

	// pak이 아닌 파일은 마운트 실패
	E_EXPECT_FALSE(FFileSystem::Mount(Source / L"Game" / L"Content" / L"Scene.escene", Mount));
	E_EXPECT_FALSE(FFileSystem::Mount(Base / L"없음.epak", Mount));
	E_EXPECT_EQ(FFileSystem::GetMountedFileCount(), static_cast<size_t>(0));
	fs::remove_all(Base, ErrorCode);
}

E_TEST(FileSystem_ReadFileRange)
{
	// 텍스처 밉 스트리밍: 디스크 파일과 pak 항목 모두 [오프셋, 오프셋 + 크기) 범위 읽기
	const fs::path Base   = FTestRegistry::GetTempDirectory() / L"ProjectE_RangeTest";
	const fs::path Source = Base / L"Source";
	const fs::path Mount  = Base / L"Mounted";
	std::error_code ErrorCode;
	fs::remove_all(Base, ErrorCode);
	fs::create_directories(Source / L"Cooked", ErrorCode);
	std::vector<uint8> Binary(5000);
	for (size_t Index = 0; Index < Binary.size(); ++Index)
	{
		Binary[Index] = static_cast<uint8>(Index * 7 + 3);
	}
	const fs::path File = Source / L"Cooked" / L"Brick.png.color.etex";
	{
		std::ofstream Stream(File, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Binary.data()), static_cast<std::streamsize>(Binary.size()));
	}
	// 앞에 다른 항목을 두어 pak 안 오프셋이 0이 아니게
	WriteText(Source / L"A.txt", "first entry");

	const auto Expect = [&](const fs::path& Path, uint64 Offset, uint64 Size) {
		std::vector<uint8> Range;
		const bool         bOk = FFileSystem::ReadFileRange(Path, Offset, Size, Range);
		return bOk && Range.size() == Size && std::equal(Range.begin(), Range.end(), Binary.begin() + static_cast<std::ptrdiff_t>(Offset));
	};
	std::vector<uint8> Range;
	E_EXPECT_TRUE(Expect(File, 0, 14));
	E_EXPECT_TRUE(Expect(File, 1234, 2000));
	E_EXPECT_TRUE(Expect(File, 4990, 10)); // 끝까지
	E_EXPECT_TRUE(Expect(File, 5000, 0));
	E_EXPECT_FALSE(FFileSystem::ReadFileRange(File, 4990, 11, Range)); // 파일 밖
	E_EXPECT_FALSE(FFileSystem::ReadFileRange(Source / L"None.etex", 0, 1, Range));
	E_EXPECT_TRUE(FFileSystem::GetFileSize(File).value_or(0) == 5000);

	std::vector<std::pair<std::string, fs::path>> Files = { { FFileSystem::MakeKey(Source / L"A.txt", Source), Source / L"A.txt" },
	                                                        { FFileSystem::MakeKey(File, Source), File } };
	std::string Error;
	E_EXPECT_TRUE(FPakWriter::Write(Base / L"Range.epak", Files, Error));
	E_EXPECT_TRUE(FFileSystem::Mount(Base / L"Range.epak", Mount));
	const fs::path InPak = Mount / L"Cooked" / L"Brick.png.color.etex";
	E_EXPECT_TRUE(FFileSystem::IsInPak(InPak));
	E_EXPECT_TRUE(Expect(InPak, 0, 14));
	E_EXPECT_TRUE(Expect(InPak, 1234, 2000));
	E_EXPECT_TRUE(Expect(InPak, 4990, 10));
	E_EXPECT_FALSE(FFileSystem::ReadFileRange(InPak, 4990, 11, Range)); // 항목 밖 (다음 항목/색인을 읽지 않는다)
	E_EXPECT_TRUE(FFileSystem::GetFileSize(InPak).value_or(0) == 5000);
	FFileSystem::UnmountAll();
	fs::remove_all(Base, ErrorCode);
}
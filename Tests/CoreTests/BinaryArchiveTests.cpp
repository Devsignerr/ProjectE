#include "Core/Math/Math.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>

E_TEST(BinaryArchive_RoundTrip)
{
	FBinaryWriter Writer;
	Writer.Write<uint32>(0xDEADBEEFu);
	Writer.Write<float>(1.5f);
	Writer.Write(FVector3(1.0f, 2.0f, 3.0f));
	Writer.WriteString("한글 문자열");
	Writer.WriteString("");
	Writer.WriteArray(std::vector<int32>{ 1, -2, 3 });
	Writer.WriteArray(std::vector<uint8>{});

	const std::vector<uint8>& Bytes = Writer.GetBuffer();
	FBinaryReader             Reader(Bytes.data(), Bytes.size());
	E_EXPECT_EQ(Reader.Read<uint32>(), 0xDEADBEEFu);
	E_EXPECT_NEAR(Reader.Read<float>(), 1.5f, 0.0f);
	E_EXPECT_EQUALS(Reader.Read<FVector3>(), FVector3(1.0f, 2.0f, 3.0f), 0.0f);
	E_EXPECT_TRUE(Reader.ReadString() == "한글 문자열");
	E_EXPECT_TRUE(Reader.ReadString().empty());
	const std::vector<int32> Ints = Reader.ReadArray<int32>();
	E_EXPECT_EQ(Ints.size(), static_cast<size_t>(3));
	E_EXPECT_TRUE(Ints.size() == 3 && Ints[1] == -2);
	E_EXPECT_TRUE(Reader.ReadArray<uint8>().empty());
	E_EXPECT_TRUE(Reader.IsOk());
	E_EXPECT_TRUE(Reader.IsAtEnd());
}

E_TEST(BinaryArchive_TruncatedDataFailsSafely)
{
	FBinaryWriter Writer;
	Writer.WriteString("abcdef");
	Writer.WriteArray(std::vector<uint32>{ 1, 2, 3, 4 });
	const std::vector<uint8>& Bytes = Writer.GetBuffer();

	// 중간에서 잘린 데이터: 실패 상태가 되고 이후 읽기는 기본값
	FBinaryReader Reader(Bytes.data(), Bytes.size() - 5);
	E_EXPECT_TRUE(Reader.ReadString() == "abcdef");
	E_EXPECT_TRUE(Reader.ReadArray<uint32>().empty());
	E_EXPECT_FALSE(Reader.IsOk());
	E_EXPECT_EQ(Reader.Read<uint32>(), 0u);

	// 거대한 길이 필드 (손상) → 할당 없이 실패
	FBinaryWriter Bad;
	Bad.Write<uint32>(0xFFFFFFF0u);
	FBinaryReader BadReader(Bad.GetBuffer().data(), Bad.GetBuffer().size());
	E_EXPECT_TRUE(BadReader.ReadArray<uint32>().empty());
	E_EXPECT_FALSE(BadReader.IsOk());

	FBinaryReader Empty;
	E_EXPECT_TRUE(Empty.ReadString().empty());
	E_EXPECT_FALSE(Empty.IsOk());
}

E_TEST(BinaryArchive_FileSaveLoad)
{
	const std::filesystem::path Path = std::filesystem::temp_directory_path() / L"ProjectE_테스트_Archive" / L"Data.bin";
	FBinaryWriter               Writer;
	Writer.WriteArray(std::vector<float>{ 0.25f, 0.5f });
	E_EXPECT_TRUE(Writer.SaveToFile(Path));
	E_EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(Path).concat(L".tmp")));

	std::vector<uint8> Bytes;
	E_EXPECT_TRUE(ReadFileBytes(Path, Bytes));
	E_EXPECT_TRUE(Bytes == Writer.GetBuffer());
	std::filesystem::remove_all(Path.parent_path());

	E_EXPECT_FALSE(ReadFileBytes(Path, Bytes));
}

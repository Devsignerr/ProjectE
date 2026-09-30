#include "Core/Platform/ExecutableResources.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace
{
	std::wstring QueryVersionString(const std::vector<uint8>& Block, const wchar_t* Key)
	{
		wchar_t* Value  = nullptr;
		UINT     Length = 0;
		const std::wstring SubBlock = std::wstring(L"\\StringFileInfo\\040904B0\\") + Key;
		if (!VerQueryValueW(Block.data(), SubBlock.c_str(), reinterpret_cast<void**>(&Value), &Length) || Value == nullptr)
		{
			return {};
		}
		return std::wstring(Value);
	}
} // namespace

E_TEST(ExecutableResources_ParseVersion)
{
	uint16 Parts[4] = {};
	E_EXPECT_TRUE(FExecutableResources::ParseVersion("1.2.3", Parts));
	E_EXPECT_EQ(Parts[0], static_cast<uint16>(1));
	E_EXPECT_EQ(Parts[1], static_cast<uint16>(2));
	E_EXPECT_EQ(Parts[2], static_cast<uint16>(3));
	E_EXPECT_EQ(Parts[3], static_cast<uint16>(0));
	E_EXPECT_TRUE(FExecutableResources::ParseVersion("10.0.65535.7", Parts));
	E_EXPECT_EQ(Parts[2], static_cast<uint16>(65535));

	E_EXPECT_FALSE(FExecutableResources::ParseVersion("", Parts));
	E_EXPECT_FALSE(FExecutableResources::ParseVersion("1..2", Parts));
	E_EXPECT_FALSE(FExecutableResources::ParseVersion("1.2.", Parts));
	E_EXPECT_FALSE(FExecutableResources::ParseVersion("1.2.3.4.5", Parts));
	E_EXPECT_FALSE(FExecutableResources::ParseVersion("1.a", Parts));
	E_EXPECT_FALSE(FExecutableResources::ParseVersion("70000", Parts));
}

E_TEST(ExecutableResources_IconFromRgba)
{
	// 8x8: 왼쪽 절반 불투명 빨강, 오른쪽 절반 투명
	std::vector<uint8> Rgba(8 * 8 * 4, 0);
	for (uint32 Y = 0; Y < 8; ++Y)
	{
		for (uint32 X = 0; X < 4; ++X)
		{
			uint8* Pixel = &Rgba[(Y * 8 + X) * 4];
			Pixel[0] = 255;
			Pixel[3] = 255;
		}
	}
	const std::vector<FIconEntry> Icon = FExecutableResources::MakeIconFromRgba(8, 8, Rgba.data());
	E_EXPECT_EQ(Icon.size(), static_cast<size_t>(5));
	E_EXPECT_EQ(Icon[0].Size, 256u);
	E_EXPECT_EQ(Icon[4].Size, 16u);

	// 16x16 항목: BITMAPINFOHEADER(40) + BGRA + AND 마스크(행 4바이트)
	const FIconEntry& Small = Icon[4];
	E_EXPECT_EQ(Small.Data.size(), static_cast<size_t>(40 + 16 * 16 * 4 + 4 * 16));
	// 첫 픽셀(아래 행 왼쪽) = 불투명 빨강 BGRA, 마지막 픽셀 = 투명
	E_EXPECT_EQ(Small.Data[40 + 2], static_cast<uint8>(255));
	E_EXPECT_EQ(Small.Data[40 + 3], static_cast<uint8>(255));
	E_EXPECT_EQ(Small.Data[40 + 16 * 16 * 4 - 1], static_cast<uint8>(0));
	// AND 마스크 첫 행: 왼쪽 8픽셀 불투명(0), 오른쪽 8픽셀 투명(1)
	E_EXPECT_EQ(Small.Data[40 + 16 * 16 * 4 + 0], static_cast<uint8>(0x00));
	E_EXPECT_EQ(Small.Data[40 + 16 * 16 * 4 + 1], static_cast<uint8>(0xFF));

	E_EXPECT_TRUE(FExecutableResources::MakeIconFromRgba(0, 8, Rgba.data()).empty());
}

E_TEST(ExecutableResources_ParseIco)
{
	// 항목 2개짜리 ICO: 헤더 6 + 디렉터리 16×2 + 데이터
	std::vector<uint8> File = { 0, 0, 1, 0, 2, 0 };
	auto AppendEntry = [&File](uint8 Dimension, uint32 Bytes, uint32 Offset)
	{
		const uint8 Entry[16] = { Dimension, Dimension, 0, 0, 1, 0, 32, 0,
		                          static_cast<uint8>(Bytes), 0, 0, 0, static_cast<uint8>(Offset), 0, 0, 0 };
		File.insert(File.end(), Entry, Entry + 16);
	};
	AppendEntry(0, 3, 38);  // 256
	AppendEntry(16, 2, 41); // 16
	File.insert(File.end(), { 0xA, 0xB, 0xC, 0xD, 0xE });

	std::vector<FIconEntry> Entries;
	E_EXPECT_TRUE(FExecutableResources::ParseIco(File, Entries));
	E_EXPECT_EQ(Entries.size(), static_cast<size_t>(2));
	E_EXPECT_EQ(Entries[0].Size, 256u);
	E_EXPECT_EQ(Entries[0].Data.size(), static_cast<size_t>(3));
	E_EXPECT_EQ(Entries[1].Size, 16u);
	E_EXPECT_EQ(Entries[1].Data[1], static_cast<uint8>(0xE));

	// 범위를 넘는 항목/잘못된 머리는 실패
	std::vector<uint8> Truncated = File;
	Truncated.pop_back();
	E_EXPECT_FALSE(FExecutableResources::ParseIco(Truncated, Entries));
	E_EXPECT_FALSE(FExecutableResources::ParseIco({ 0, 0, 2, 0, 1, 0 }, Entries));
}

E_TEST(ExecutableResources_StampCopiedExecutable)
{
	// 이 테스트 실행 파일의 복사본에 아이콘/버전을 써 넣고 Windows API로 다시 읽는다
	wchar_t     ModulePath[MAX_PATH * 4];
	const DWORD Length = GetModuleFileNameW(nullptr, ModulePath, static_cast<DWORD>(std::size(ModulePath)));
	const std::filesystem::path Source(ModulePath, ModulePath + Length);
	const std::filesystem::path Copy = FTestRegistry::GetTempDirectory() / L"ProjectE_StampTest.exe";
	std::error_code ErrorCode;
	std::filesystem::copy_file(Source, Copy, std::filesystem::copy_options::overwrite_existing, ErrorCode);
	E_EXPECT_FALSE(static_cast<bool>(ErrorCode));

	std::vector<uint8> Rgba(32 * 32 * 4, 200);
	FExecutableVersionInfo Info;
	Info.ProductName      = "샘플 게임";
	Info.FileDescription  = "샘플 게임";
	Info.CompanyName      = "Studio";
	Info.Version          = "1.2.3";
	Info.OriginalFilename = "Sample.exe";
	std::string Error;
	E_EXPECT_TRUE(FExecutableResources::Stamp(Copy, FExecutableResources::MakeIconFromRgba(32, 32, Rgba.data()), Info, Error));

	// 버전 정보
	const DWORD BlockSize = GetFileVersionInfoSizeW(Copy.c_str(), nullptr);
	E_EXPECT_TRUE(BlockSize > 0);
	std::vector<uint8> Block(BlockSize);
	E_EXPECT_TRUE(GetFileVersionInfoW(Copy.c_str(), 0, BlockSize, Block.data()) != FALSE);
	E_EXPECT_TRUE(QueryVersionString(Block, L"ProductName") == FStringConv::ToWide("샘플 게임"));
	E_EXPECT_TRUE(QueryVersionString(Block, L"CompanyName") == L"Studio");
	E_EXPECT_TRUE(QueryVersionString(Block, L"FileVersion") == L"1.2.3.0");
	E_EXPECT_TRUE(QueryVersionString(Block, L"OriginalFilename") == L"Sample.exe");
	E_EXPECT_TRUE(QueryVersionString(Block, L"LegalCopyright").empty()); // 빈 값은 기록하지 않는다

	VS_FIXEDFILEINFO* Fixed = nullptr;
	UINT              FixedLength = 0;
	E_EXPECT_TRUE(VerQueryValueW(Block.data(), L"\\", reinterpret_cast<void**>(&Fixed), &FixedLength) != FALSE);
	E_EXPECT_TRUE(Fixed != nullptr && Fixed->dwFileVersionMS == ((1u << 16) | 2u) && Fixed->dwFileVersionLS == (3u << 16));

	// 아이콘 그룹 1을 창 아이콘처럼 읽을 수 있다
	const HMODULE Module = LoadLibraryExW(Copy.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	E_EXPECT_TRUE(Module != nullptr);
	if (Module != nullptr)
	{
		E_EXPECT_TRUE(FindResourceW(Module, MAKEINTRESOURCEW(FExecutableResources::IconGroupId), RT_GROUP_ICON) != nullptr);
		const HANDLE Icon = LoadImageW(Module, MAKEINTRESOURCEW(FExecutableResources::IconGroupId), IMAGE_ICON, 32, 32, 0);
		E_EXPECT_TRUE(Icon != nullptr);
		if (Icon != nullptr)
		{
			DestroyIcon(static_cast<HICON>(Icon));
		}
		FreeLibrary(Module);
	}
	std::filesystem::remove(Copy, ErrorCode);
}

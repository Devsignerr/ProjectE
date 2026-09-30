#include "Core/Platform/ExecutableResources.h"

#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <winver.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace
{
	// ---- 리틀 엔디언 읽기/쓰기
	uint16 ReadU16(const std::vector<uint8>& Bytes, size_t Offset)
	{
		return static_cast<uint16>(Bytes[Offset] | (Bytes[Offset + 1] << 8));
	}

	uint32 ReadU32(const std::vector<uint8>& Bytes, size_t Offset)
	{
		return static_cast<uint32>(ReadU16(Bytes, Offset)) | (static_cast<uint32>(ReadU16(Bytes, Offset + 2)) << 16);
	}

	void WriteU8(std::vector<uint8>& Bytes, uint8 Value)
	{
		Bytes.push_back(Value);
	}

	void WriteU16(std::vector<uint8>& Bytes, uint16 Value)
	{
		Bytes.push_back(static_cast<uint8>(Value & 0xFF));
		Bytes.push_back(static_cast<uint8>(Value >> 8));
	}

	void WriteU32(std::vector<uint8>& Bytes, uint32 Value)
	{
		WriteU16(Bytes, static_cast<uint16>(Value & 0xFFFF));
		WriteU16(Bytes, static_cast<uint16>(Value >> 16));
	}

	void PatchU16(std::vector<uint8>& Bytes, size_t Offset, uint16 Value)
	{
		Bytes[Offset]     = static_cast<uint8>(Value & 0xFF);
		Bytes[Offset + 1] = static_cast<uint8>(Value >> 8);
	}

	// ---- 아이콘: 면적 평균 축소 (알파 미리 곱하기) → 32비트 BMP DIB (XOR 상하 반전 + AND 마스크)
	FIconEntry MakeBmpEntry(uint32 SourceWidth, uint32 SourceHeight, const uint8* Rgba, uint32 Size)
	{
		std::vector<uint8> Pixels(static_cast<size_t>(Size) * Size * 4); // RGBA, 상단 행부터
		for (uint32 Y = 0; Y < Size; ++Y)
		{
			const uint32 SourceY0 = Y * SourceHeight / Size;
			const uint32 SourceY1 = std::max(SourceY0 + 1, (Y + 1) * SourceHeight / Size);
			for (uint32 X = 0; X < Size; ++X)
			{
				const uint32 SourceX0 = X * SourceWidth / Size;
				const uint32 SourceX1 = std::max(SourceX0 + 1, (X + 1) * SourceWidth / Size);
				double Sum[4] = {};
				uint32 Count  = 0;
				for (uint32 SourceY = SourceY0; SourceY < SourceY1; ++SourceY)
				{
					for (uint32 SourceX = SourceX0; SourceX < SourceX1; ++SourceX)
					{
						const uint8* Pixel = Rgba + (static_cast<size_t>(SourceY) * SourceWidth + SourceX) * 4;
						const double Alpha = Pixel[3] / 255.0;
						Sum[0] += Pixel[0] * Alpha;
						Sum[1] += Pixel[1] * Alpha;
						Sum[2] += Pixel[2] * Alpha;
						Sum[3] += Alpha;
						++Count;
					}
				}
				uint8* Out = &Pixels[(static_cast<size_t>(Y) * Size + X) * 4];
				const double AverageAlpha = Sum[3] / Count;
				for (int Channel = 0; Channel < 3; ++Channel)
				{
					Out[Channel] = Sum[3] > 0.0 ? static_cast<uint8>(std::clamp(Sum[Channel] / Sum[3] + 0.5, 0.0, 255.0)) : 0;
				}
				Out[3] = static_cast<uint8>(std::clamp(AverageAlpha * 255.0 + 0.5, 0.0, 255.0));
			}
		}

		FIconEntry Entry;
		Entry.Size     = Size;
		Entry.BitCount = 32;
		const uint32 MaskRowBytes = ((Size + 31) / 32) * 4;
		const uint32 ImageBytes   = Size * Size * 4 + MaskRowBytes * Size;
		std::vector<uint8>& Data  = Entry.Data;
		Data.reserve(40 + ImageBytes);
		// BITMAPINFOHEADER: 높이는 XOR + AND 두 장이라 2배
		WriteU32(Data, 40);
		WriteU32(Data, Size);
		WriteU32(Data, Size * 2);
		WriteU16(Data, 1);  // biPlanes
		WriteU16(Data, 32); // biBitCount
		WriteU32(Data, 0);  // BI_RGB
		WriteU32(Data, ImageBytes);
		WriteU32(Data, 0);
		WriteU32(Data, 0);
		WriteU32(Data, 0);
		WriteU32(Data, 0);
		// XOR: 아래 행부터 BGRA
		for (uint32 Row = 0; Row < Size; ++Row)
		{
			const uint8* Line = &Pixels[static_cast<size_t>(Size - 1 - Row) * Size * 4];
			for (uint32 X = 0; X < Size; ++X)
			{
				WriteU8(Data, Line[X * 4 + 2]);
				WriteU8(Data, Line[X * 4 + 1]);
				WriteU8(Data, Line[X * 4 + 0]);
				WriteU8(Data, Line[X * 4 + 3]);
			}
		}
		// AND 마스크: 투명(알파 < 128)이면 1 — 알파를 모르는 옛 경로용
		for (uint32 Row = 0; Row < Size; ++Row)
		{
			const uint8*       Line = &Pixels[static_cast<size_t>(Size - 1 - Row) * Size * 4];
			std::vector<uint8> MaskRow(MaskRowBytes, 0);
			for (uint32 X = 0; X < Size; ++X)
			{
				if (Line[X * 4 + 3] < 128)
				{
					MaskRow[X / 8] |= static_cast<uint8>(0x80 >> (X % 8));
				}
			}
			Data.insert(Data.end(), MaskRow.begin(), MaskRow.end());
		}
		return Entry;
	}

	// ---- 버전 리소스 노드: { WORD wLength; WORD wValueLength; WORD wType; WCHAR szKey[]; 패딩; Value; 패딩; Children }
	void Align4(std::vector<uint8>& Bytes)
	{
		while (Bytes.size() % 4 != 0)
		{
			Bytes.push_back(0);
		}
	}

	void WriteWideString(std::vector<uint8>& Bytes, const std::wstring& Text)
	{
		for (const wchar_t Character : Text)
		{
			WriteU16(Bytes, static_cast<uint16>(Character));
		}
		WriteU16(Bytes, 0);
	}

	// 노드 머리 + 키를 쓰고 시작 위치를 돌려준다. 값은 호출자가 이어서 쓴다
	size_t BeginNode(std::vector<uint8>& Bytes, const wchar_t* Key, uint16 ValueLength, uint16 Type)
	{
		Align4(Bytes);
		const size_t Start = Bytes.size();
		WriteU16(Bytes, 0); // wLength (EndNode에서 채움)
		WriteU16(Bytes, ValueLength);
		WriteU16(Bytes, Type);
		WriteWideString(Bytes, Key);
		Align4(Bytes);
		return Start;
	}

	void EndNode(std::vector<uint8>& Bytes, size_t Start)
	{
		PatchU16(Bytes, Start, static_cast<uint16>(Bytes.size() - Start));
	}

	void WriteStringNode(std::vector<uint8>& Bytes, const wchar_t* Key, const std::string& Value)
	{
		if (Value.empty())
		{
			return;
		}
		const std::wstring Wide  = FStringConv::ToWide(Value);
		const size_t       Start = BeginNode(Bytes, Key, static_cast<uint16>(Wide.size() + 1), 1); // 텍스트 값 길이 = 글자 수(널 포함)
		WriteWideString(Bytes, Wide);
		EndNode(Bytes, Start);
	}
} // namespace

bool FExecutableResources::ParseIco(const std::vector<uint8>& FileBytes, std::vector<FIconEntry>& OutEntries)
{
	OutEntries.clear();
	if (FileBytes.size() < 6 || ReadU16(FileBytes, 0) != 0 || ReadU16(FileBytes, 2) != 1)
	{
		return false;
	}
	const uint16 Count = ReadU16(FileBytes, 4);
	if (Count == 0 || FileBytes.size() < 6 + static_cast<size_t>(Count) * 16)
	{
		return false;
	}
	for (uint16 Index = 0; Index < Count; ++Index)
	{
		const size_t EntryOffset = 6 + static_cast<size_t>(Index) * 16;
		const uint32 Bytes       = ReadU32(FileBytes, EntryOffset + 8);
		const uint32 Offset      = ReadU32(FileBytes, EntryOffset + 12);
		if (Bytes == 0 || static_cast<size_t>(Offset) + Bytes > FileBytes.size())
		{
			OutEntries.clear();
			return false;
		}
		FIconEntry Entry;
		Entry.Size     = FileBytes[EntryOffset] == 0 ? 256u : FileBytes[EntryOffset];
		Entry.BitCount = ReadU16(FileBytes, EntryOffset + 6);
		Entry.Data.assign(FileBytes.begin() + Offset, FileBytes.begin() + Offset + Bytes);
		OutEntries.push_back(std::move(Entry));
	}
	return true;
}

std::vector<FIconEntry> FExecutableResources::MakeIconFromRgba(uint32 Width, uint32 Height, const uint8* Rgba)
{
	std::vector<FIconEntry> Entries;
	if (Width == 0 || Height == 0 || Rgba == nullptr)
	{
		return Entries;
	}
	for (const uint32 Size : { 256u, 64u, 48u, 32u, 16u })
	{
		Entries.push_back(MakeBmpEntry(Width, Height, Rgba, Size));
	}
	return Entries;
}

bool FExecutableResources::ParseVersion(const std::string& Text, uint16 OutParts[4])
{
	uint16 Parts[4] = {};
	size_t PartCount = 0;
	size_t Begin     = 0;
	while (Begin <= Text.size())
	{
		const size_t End = std::min(Text.find('.', Begin), Text.size());
		if (PartCount == 4 || End == Begin)
		{
			return false;
		}
		uint32     Value  = 0;
		const auto Result = std::from_chars(Text.data() + Begin, Text.data() + End, Value);
		if (Result.ec != std::errc() || Result.ptr != Text.data() + End || Value > 0xFFFF)
		{
			return false;
		}
		Parts[PartCount++] = static_cast<uint16>(Value);
		Begin              = End + 1;
	}
	std::memcpy(OutParts, Parts, sizeof(Parts));
	return PartCount > 0;
}

std::vector<uint8> FExecutableResources::BuildVersionResource(const FExecutableVersionInfo& Info)
{
	uint16 Parts[4] = {};
	ParseVersion(Info.Version, Parts);
	const std::string VersionText = std::format("{}.{}.{}.{}", Parts[0], Parts[1], Parts[2], Parts[3]);

	VS_FIXEDFILEINFO Fixed{};
	Fixed.dwSignature        = VS_FFI_SIGNATURE;
	Fixed.dwStrucVersion     = VS_FFI_STRUCVERSION;
	Fixed.dwFileVersionMS    = (static_cast<DWORD>(Parts[0]) << 16) | Parts[1];
	Fixed.dwFileVersionLS    = (static_cast<DWORD>(Parts[2]) << 16) | Parts[3];
	Fixed.dwProductVersionMS = Fixed.dwFileVersionMS;
	Fixed.dwProductVersionLS = Fixed.dwFileVersionLS;
	Fixed.dwFileFlagsMask    = VS_FFI_FILEFLAGSMASK;
	Fixed.dwFileOS           = VOS_NT_WINDOWS32;
	Fixed.dwFileType         = VFT_APP;

	std::vector<uint8> Bytes;
	const size_t Root = BeginNode(Bytes, L"VS_VERSION_INFO", static_cast<uint16>(sizeof(Fixed)), 0);
	const uint8* FixedBytes = reinterpret_cast<const uint8*>(&Fixed);
	Bytes.insert(Bytes.end(), FixedBytes, FixedBytes + sizeof(Fixed));

	const size_t StringFileInfo = BeginNode(Bytes, L"StringFileInfo", 0, 1);
	const size_t StringTable    = BeginNode(Bytes, L"040904B0", 0, 1); // en-US, 유니코드(1200)
	WriteStringNode(Bytes, L"CompanyName", Info.CompanyName);
	WriteStringNode(Bytes, L"FileDescription", Info.FileDescription);
	WriteStringNode(Bytes, L"FileVersion", VersionText);
	WriteStringNode(Bytes, L"InternalName", Info.InternalName);
	WriteStringNode(Bytes, L"LegalCopyright", Info.LegalCopyright);
	WriteStringNode(Bytes, L"OriginalFilename", Info.OriginalFilename);
	WriteStringNode(Bytes, L"ProductName", Info.ProductName);
	WriteStringNode(Bytes, L"ProductVersion", VersionText);
	EndNode(Bytes, StringTable);
	EndNode(Bytes, StringFileInfo);

	const size_t VarFileInfo = BeginNode(Bytes, L"VarFileInfo", 0, 1);
	const size_t Translation = BeginNode(Bytes, L"Translation", 4, 0);
	WriteU16(Bytes, 0x0409);
	WriteU16(Bytes, 1200);
	EndNode(Bytes, Translation);
	EndNode(Bytes, VarFileInfo);

	EndNode(Bytes, Root);
	return Bytes;
}

bool FExecutableResources::Stamp(const std::filesystem::path& ExecutablePath, const std::vector<FIconEntry>& Icon,
                                 const FExecutableVersionInfo& Version, std::string& OutError)
{
	const HANDLE Update = BeginUpdateResourceW(ExecutablePath.c_str(), FALSE);
	if (Update == nullptr)
	{
		OutError = std::format("BeginUpdateResource 실패 (오류 코드 {})", GetLastError());
		return false;
	}

	const WORD Language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
	bool       bOk      = true;
	auto Put = [&](LPCWSTR Type, WORD Id, std::vector<uint8>& Data)
	{
		if (bOk && !UpdateResourceW(Update, Type, MAKEINTRESOURCEW(Id), Language, Data.data(), static_cast<DWORD>(Data.size())))
		{
			OutError = std::format("UpdateResource 실패 (오류 코드 {})", GetLastError());
			bOk      = false;
		}
	};

	if (!Icon.empty())
	{
		// RT_ICON 1..N + RT_GROUP_ICON(GRPICONDIR: 항목마다 오프셋 대신 RT_ICON ID)
		std::vector<uint8> Group;
		WriteU16(Group, 0);
		WriteU16(Group, 1);
		WriteU16(Group, static_cast<uint16>(Icon.size()));
		for (size_t Index = 0; Index < Icon.size(); ++Index)
		{
			const FIconEntry& Entry = Icon[Index];
			std::vector<uint8> Data  = Entry.Data;
			Put(RT_ICON, static_cast<WORD>(Index + 1), Data);

			const uint8 Dimension = Entry.Size >= 256 ? 0 : static_cast<uint8>(Entry.Size);
			WriteU8(Group, Dimension);
			WriteU8(Group, Dimension);
			WriteU8(Group, 0); // 색 수 (32비트는 0)
			WriteU8(Group, 0);
			WriteU16(Group, 1);
			WriteU16(Group, Entry.BitCount);
			WriteU32(Group, static_cast<uint32>(Entry.Data.size()));
			WriteU16(Group, static_cast<uint16>(Index + 1));
		}
		Put(RT_GROUP_ICON, IconGroupId, Group);
	}

	std::vector<uint8> VersionBytes = BuildVersionResource(Version);
	Put(RT_VERSION, 1, VersionBytes);

	if (!EndUpdateResourceW(Update, bOk ? FALSE : TRUE) && bOk)
	{
		OutError = std::format("EndUpdateResource 실패 (오류 코드 {})", GetLastError());
		bOk      = false;
	}
	return bOk;
}

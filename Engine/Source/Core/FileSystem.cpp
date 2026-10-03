#include "Core/FileSystem.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <cwctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
	constexpr size_t GHeaderSize = 24; // Magic, Version, Count, Reserved (u32 ×4) + IndexOffset (u64)

	struct FPakEntry
	{
		uint64 Offset = 0;
		uint64 Size   = 0;
		uint64 Hash   = 0;
	};

	struct FMountedPak
	{
		std::filesystem::path                      File;
		std::filesystem::path                      MountRoot;
		std::filesystem::file_time_type            WriteTime;
		std::unordered_map<std::string, FPakEntry> Entries;
		std::ifstream                              Stream;
		std::mutex                                 StreamMutex; // 읽기(seek + read) 직렬화
	};

	std::vector<std::unique_ptr<FMountedPak>>& GetPaks()
	{
		static std::vector<std::unique_ptr<FMountedPak>> Paks;
		return Paks;
	}

	uint64 HashBytes(const uint8* Data, size_t Size)
	{
		uint64 Hash = 1469598103934665603ull; // FNV-1a 64
		for (size_t Index = 0; Index < Size; ++Index)
		{
			Hash ^= Data[Index];
			Hash *= 1099511628211ull;
		}
		return Hash;
	}

	std::wstring NormalizeLower(const std::filesystem::path& Path)
	{
		std::error_code ErrorCode;
		std::filesystem::path Absolute = std::filesystem::absolute(Path, ErrorCode);
		std::wstring Text = (ErrorCode ? Path : Absolute).lexically_normal().generic_wstring();
		for (wchar_t& Character : Text)
		{
			Character = static_cast<wchar_t>(std::towlower(Character));
		}
		return Text;
	}

	template <typename T>
	bool ReadValue(std::ifstream& Stream, T& OutValue)
	{
		return static_cast<bool>(Stream.read(reinterpret_cast<char*>(&OutValue), sizeof(T)));
	}

	template <typename T>
	void WriteValue(std::ofstream& Stream, const T& Value)
	{
		Stream.write(reinterpret_cast<const char*>(&Value), sizeof(T));
	}

	// 키가 있는 pak 항목 (없으면 nullptr)
	FMountedPak* FindEntry(const std::filesystem::path& Path, const FPakEntry*& OutEntry)
	{
		for (const std::unique_ptr<FMountedPak>& Pak : GetPaks())
		{
			const std::string Key = FFileSystem::MakeKey(Path, Pak->MountRoot);
			if (Key.empty())
			{
				continue;
			}
			if (const auto It = Pak->Entries.find(Key); It != Pak->Entries.end())
			{
				OutEntry = &It->second;
				return Pak.get();
			}
		}
		return nullptr;
	}

	bool ReadDiskFile(const std::filesystem::path& Path, std::vector<uint8>& OutBytes)
	{
		OutBytes.clear();
		std::ifstream File(Path, std::ios::binary | std::ios::ate);
		if (!File)
		{
			return false;
		}
		const std::streamsize FileSize = File.tellg();
		if (FileSize < 0)
		{
			return false;
		}
		File.seekg(0, std::ios::beg);
		OutBytes.resize(static_cast<size_t>(FileSize));
		return FileSize == 0 || static_cast<bool>(File.read(reinterpret_cast<char*>(OutBytes.data()), FileSize));
	}
} // namespace

std::string FFileSystem::MakeKey(const std::filesystem::path& Path, const std::filesystem::path& MountRoot)
{
	const std::wstring Full = NormalizeLower(Path);
	std::wstring       Root = NormalizeLower(MountRoot);
	if (Root.empty())
	{
		return {};
	}
	if (Root.back() != L'/')
	{
		Root += L'/';
	}
	if (Full.size() <= Root.size() || Full.compare(0, Root.size(), Root) != 0)
	{
		return {};
	}
	return FStringConv::ToUtf8(Full.substr(Root.size()));
}

bool FFileSystem::Mount(const std::filesystem::path& PakFile, const std::filesystem::path& MountRoot)
{
	auto Pak       = std::make_unique<FMountedPak>();
	Pak->File      = PakFile;
	Pak->MountRoot = MountRoot;
	const std::string PakName = FStringConv::ToUtf8(PakFile.wstring());

	std::error_code ErrorCode;
	Pak->WriteTime = std::filesystem::last_write_time(PakFile, ErrorCode);
	Pak->Stream.open(PakFile, std::ios::binary);
	if (ErrorCode || !Pak->Stream)
	{
		E_LOG(LogCore, Error, "pak을 열 수 없습니다: {}", PakName);
		return false;
	}
	const uint64 FileSize = std::filesystem::file_size(PakFile, ErrorCode);

	uint32 Magic = 0, Version = 0, Count = 0, Reserved = 0;
	uint64 IndexOffset = 0;
	std::ifstream& Stream = Pak->Stream;
	if (!ReadValue(Stream, Magic) || !ReadValue(Stream, Version) || !ReadValue(Stream, Count) || !ReadValue(Stream, Reserved) ||
	    !ReadValue(Stream, IndexOffset) || Magic != FPakWriter::Magic || Version != FPakWriter::Version || IndexOffset > FileSize)
	{
		E_LOG(LogCore, Error, "pak 머리가 잘못되었거나 버전이 다릅니다 (버전 {} / 기대 {}): {}", Version, FPakWriter::Version, PakName);
		return false;
	}

	Stream.seekg(static_cast<std::streamoff>(IndexOffset));
	Pak->Entries.reserve(Count);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		uint16 KeyLength = 0;
		if (!ReadValue(Stream, KeyLength))
		{
			break;
		}
		std::string Key(KeyLength, '\0');
		FPakEntry   Entry;
		if (!Stream.read(Key.data(), KeyLength) || !ReadValue(Stream, Entry.Offset) || !ReadValue(Stream, Entry.Size) || !ReadValue(Stream, Entry.Hash) ||
		    Entry.Offset < GHeaderSize || Entry.Offset + Entry.Size > IndexOffset)
		{
			E_LOG(LogCore, Error, "pak 색인이 손상되었습니다 (항목 {}): {}", Index, PakName);
			return false;
		}
		Pak->Entries.emplace(std::move(Key), Entry);
	}
	if (Pak->Entries.size() != Count)
	{
		E_LOG(LogCore, Error, "pak 색인이 손상되었습니다 (항목 {}/{}): {}", Pak->Entries.size(), Count, PakName);
		return false;
	}

	E_LOG(LogCore, Display, "pak 마운트: {} (파일 {}개)", PakName, Count);
	GetPaks().push_back(std::move(Pak));
	return true;
}

void FFileSystem::UnmountAll()
{
	GetPaks().clear();
}

size_t FFileSystem::GetMountedFileCount()
{
	size_t Count = 0;
	for (const std::unique_ptr<FMountedPak>& Pak : GetPaks())
	{
		Count += Pak->Entries.size();
	}
	return Count;
}

bool FFileSystem::IsInPak(const std::filesystem::path& Path)
{
	const FPakEntry* Entry = nullptr;
	return FindEntry(Path, Entry) != nullptr;
}

bool FFileSystem::Exists(const std::filesystem::path& Path)
{
	if (IsInPak(Path))
	{
		return true;
	}
	std::error_code ErrorCode;
	return std::filesystem::is_regular_file(Path, ErrorCode);
}

bool FFileSystem::ReadFile(const std::filesystem::path& Path, std::vector<uint8>& OutBytes)
{
	const FPakEntry* Entry = nullptr;
	FMountedPak*     Pak   = FindEntry(Path, Entry);
	if (Pak == nullptr)
	{
		return ReadDiskFile(Path, OutBytes);
	}

	OutBytes.resize(static_cast<size_t>(Entry->Size));
	{
		std::scoped_lock Lock(Pak->StreamMutex);
		Pak->Stream.clear();
		Pak->Stream.seekg(static_cast<std::streamoff>(Entry->Offset));
		if (Entry->Size > 0 && !Pak->Stream.read(reinterpret_cast<char*>(OutBytes.data()), static_cast<std::streamsize>(Entry->Size)))
		{
			E_LOG(LogCore, Error, "pak 항목을 읽을 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
			OutBytes.clear();
			return false;
		}
	}
	if (HashBytes(OutBytes.data(), OutBytes.size()) != Entry->Hash)
	{
		E_LOG(LogCore, Error, "pak 항목이 손상되었습니다 (해시 불일치): {}", FStringConv::ToUtf8(Path.wstring()));
		OutBytes.clear();
		return false;
	}
	return true;
}

bool FFileSystem::ReadFileRange(const std::filesystem::path& Path, uint64 Offset, uint64 Size, std::vector<uint8>& OutBytes)
{
	OutBytes.clear();
	const FPakEntry* Entry = nullptr;
	if (FMountedPak* Pak = FindEntry(Path, Entry))
	{
		if (Offset > Entry->Size || Size > Entry->Size - Offset)
		{
			return false;
		}
		OutBytes.resize(static_cast<size_t>(Size));
		std::scoped_lock Lock(Pak->StreamMutex);
		Pak->Stream.clear();
		Pak->Stream.seekg(static_cast<std::streamoff>(Entry->Offset + Offset));
		if (Size > 0 && !Pak->Stream.read(reinterpret_cast<char*>(OutBytes.data()), static_cast<std::streamsize>(Size)))
		{
			E_LOG(LogCore, Error, "pak 항목 범위를 읽을 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
			OutBytes.clear();
			return false;
		}
		return true;
	}

	std::ifstream File(Path, std::ios::binary | std::ios::ate);
	if (!File)
	{
		return false;
	}
	const std::streamsize FileSize = File.tellg();
	if (FileSize < 0 || Offset > static_cast<uint64>(FileSize) || Size > static_cast<uint64>(FileSize) - Offset)
	{
		return false;
	}
	File.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
	OutBytes.resize(static_cast<size_t>(Size));
	if (Size > 0 && !File.read(reinterpret_cast<char*>(OutBytes.data()), static_cast<std::streamsize>(Size)))
	{
		OutBytes.clear();
		return false;
	}
	return true;
}

std::optional<uint64> FFileSystem::GetFileSize(const std::filesystem::path& Path)
{
	const FPakEntry* Entry = nullptr;
	if (FindEntry(Path, Entry) != nullptr)
	{
		return Entry->Size;
	}
	std::error_code ErrorCode;
	const uintmax_t Size = std::filesystem::file_size(Path, ErrorCode);
	if (ErrorCode)
	{
		return std::nullopt;
	}
	return static_cast<uint64>(Size);
}

bool FFileSystem::ReadTextFile(const std::filesystem::path& Path, std::string& OutText)
{
	std::vector<uint8> Bytes;
	if (!ReadFile(Path, Bytes))
	{
		OutText.clear();
		return false;
	}
	OutText.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
	return true;
}

std::optional<std::filesystem::file_time_type> FFileSystem::GetLastWriteTime(const std::filesystem::path& Path)
{
	const FPakEntry* Entry = nullptr;
	if (const FMountedPak* Pak = FindEntry(Path, Entry))
	{
		return Pak->WriteTime;
	}
	std::error_code ErrorCode;
	const std::filesystem::file_time_type Time = std::filesystem::last_write_time(Path, ErrorCode);
	if (ErrorCode)
	{
		return std::nullopt;
	}
	return Time;
}

bool FPakWriter::Write(const std::filesystem::path& PakFile, const std::vector<std::pair<std::string, std::filesystem::path>>& Files,
                       std::string& OutError)
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(PakFile.parent_path(), ErrorCode);
	std::ofstream Stream(PakFile, std::ios::binary | std::ios::trunc);
	if (!Stream)
	{
		OutError = "pak 파일을 만들 수 없습니다: " + FStringConv::ToUtf8(PakFile.wstring());
		return false;
	}

	WriteValue(Stream, Magic);
	WriteValue(Stream, Version);
	WriteValue(Stream, static_cast<uint32>(Files.size()));
	WriteValue(Stream, uint32(0));
	WriteValue(Stream, uint64(0)); // 색인 오프셋 (끝에서 채움)

	struct FIndexRecord
	{
		const std::string* Key;
		FPakEntry          Entry;
	};
	std::vector<FIndexRecord>       Index;
	std::unordered_set<std::string> SeenKeys;
	Index.reserve(Files.size());
	uint64             Offset = GHeaderSize;
	std::vector<uint8> Bytes;
	for (const auto& [Key, Source] : Files)
	{
		if (Key.empty() || Key.size() > 0xFFFF || !SeenKeys.insert(Key).second)
		{
			OutError = "pak 키가 비었거나 너무 길거나 중복입니다: " + Key;
			return false;
		}
		if (!ReadDiskFile(Source, Bytes))
		{
			OutError = "pak에 넣을 파일을 읽을 수 없습니다: " + FStringConv::ToUtf8(Source.wstring());
			return false;
		}
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		Index.push_back({ &Key, { Offset, Bytes.size(), HashBytes(Bytes.data(), Bytes.size()) } });
		Offset += Bytes.size();
	}

	const uint64 IndexOffset = Offset;
	for (const FIndexRecord& Record : Index)
	{
		WriteValue(Stream, static_cast<uint16>(Record.Key->size()));
		Stream.write(Record.Key->data(), static_cast<std::streamsize>(Record.Key->size()));
		WriteValue(Stream, Record.Entry.Offset);
		WriteValue(Stream, Record.Entry.Size);
		WriteValue(Stream, Record.Entry.Hash);
	}
	Stream.seekp(16);
	WriteValue(Stream, IndexOffset);
	if (!Stream)
	{
		OutError = "pak 쓰기 실패: " + FStringConv::ToUtf8(PakFile.wstring());
		return false;
	}
	return true;
}

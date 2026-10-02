#include "Editor/GameModuleHotReload.h"

#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <format>
#include <fstream>
#include <iterator>
#include <share.h>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	double NowSeconds()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::wstring ToLower(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}

	template <typename T>
	bool ReadAt(const std::vector<uint8>& Image, size_t Offset, T& Out)
	{
		if (Offset > Image.size() || Image.size() - Offset < sizeof(T))
		{
			return false;
		}
		std::memcpy(&Out, Image.data() + Offset, sizeof(T));
		return true;
	}

	// PE 이미지에서 CodeView(RSDS) 레코드의 PDB 경로 칸 (파일 오프셋, 바이트 수 — 끝 NUL 포함)
	bool FindCodeViewPath(const std::vector<uint8>& Image, size_t& OutOffset, size_t& OutCapacity)
	{
		IMAGE_DOS_HEADER Dos{};
		if (!ReadAt(Image, 0, Dos) || Dos.e_magic != IMAGE_DOS_SIGNATURE || Dos.e_lfanew < 0)
		{
			return false;
		}
		const size_t NtOffset  = static_cast<size_t>(Dos.e_lfanew);
		DWORD        Signature = 0;
		if (!ReadAt(Image, NtOffset, Signature) || Signature != IMAGE_NT_SIGNATURE)
		{
			return false;
		}
		IMAGE_FILE_HEADER FileHeader{};
		if (!ReadAt(Image, NtOffset + sizeof(DWORD), FileHeader))
		{
			return false;
		}
		const size_t OptionalOffset = NtOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
		WORD         Magic          = 0;
		if (!ReadAt(Image, OptionalOffset, Magic))
		{
			return false;
		}
		IMAGE_DATA_DIRECTORY DebugDirectory{};
		if (Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			IMAGE_OPTIONAL_HEADER64 Optional{};
			if (!ReadAt(Image, OptionalOffset, Optional) || Optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
			{
				return false;
			}
			DebugDirectory = Optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		}
		else if (Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
		{
			IMAGE_OPTIONAL_HEADER32 Optional{};
			if (!ReadAt(Image, OptionalOffset, Optional) || Optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
			{
				return false;
			}
			DebugDirectory = Optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		}
		else
		{
			return false;
		}
		if (DebugDirectory.VirtualAddress == 0 || DebugDirectory.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
		{
			return false;
		}

		// RVA → 파일 오프셋 (섹션 표)
		const size_t SectionOffset = OptionalOffset + FileHeader.SizeOfOptionalHeader;
		size_t       DebugOffset   = 0;
		bool         bMapped       = false;
		for (WORD Index = 0; Index < FileHeader.NumberOfSections && !bMapped; ++Index)
		{
			IMAGE_SECTION_HEADER Section{};
			if (!ReadAt(Image, SectionOffset + Index * sizeof(IMAGE_SECTION_HEADER), Section))
			{
				return false;
			}
			const DWORD Extent = std::max(Section.Misc.VirtualSize, Section.SizeOfRawData);
			if (DebugDirectory.VirtualAddress >= Section.VirtualAddress && DebugDirectory.VirtualAddress < Section.VirtualAddress + Extent)
			{
				DebugOffset = static_cast<size_t>(Section.PointerToRawData) + (DebugDirectory.VirtualAddress - Section.VirtualAddress);
				bMapped     = true;
			}
		}
		if (!bMapped)
		{
			return false;
		}

		const size_t EntryCount = DebugDirectory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
		for (size_t Index = 0; Index < EntryCount; ++Index)
		{
			IMAGE_DEBUG_DIRECTORY Entry{};
			if (!ReadAt(Image, DebugOffset + Index * sizeof(IMAGE_DEBUG_DIRECTORY), Entry))
			{
				return false;
			}
			if (Entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW)
			{
				continue;
			}
			// RSDS 레코드: 'RSDS'(4) + GUID(16) + Age(4) + NUL로 끝나는 경로
			constexpr size_t HeaderSize = 24;
			DWORD            CvSignature = 0;
			if (Entry.SizeOfData <= HeaderSize || !ReadAt(Image, Entry.PointerToRawData, CvSignature) || CvSignature != 0x53445352u ||
			    static_cast<size_t>(Entry.PointerToRawData) + Entry.SizeOfData > Image.size())
			{
				continue;
			}
			OutOffset   = static_cast<size_t>(Entry.PointerToRawData) + HeaderSize;
			OutCapacity = Entry.SizeOfData - HeaderSize;
			return true;
		}
		return false;
	}

	// 빌드 출력 한 줄: UTF-8이면 그대로, 아니면 콘솔 코드 페이지(한국어 Windows는 CP949)에서 변환
	std::string ToUtf8Line(const std::string& Raw)
	{
		if (Raw.empty())
		{
			return Raw;
		}
		const int RawSize = static_cast<int>(Raw.size());
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Raw.data(), RawSize, nullptr, 0) > 0)
		{
			return Raw;
		}
		const int    WideSize = MultiByteToWideChar(CP_OEMCP, 0, Raw.data(), RawSize, nullptr, 0);
		std::wstring Wide(static_cast<size_t>(std::max(WideSize, 0)), L'\0');
		MultiByteToWideChar(CP_OEMCP, 0, Raw.data(), RawSize, Wide.data(), WideSize);
		return FStringConv::ToUtf8(Wide);
	}

	// Ninja 규칙의 msvc_deps_prefix가 ASCII가 아닌 UTF-8인가 (한국어 cl "참고: 포함 파일:"을 UTF-8 콘솔에서 구성)
	bool UsesUtf8DepsPrefix(const std::filesystem::path& BuildDirectory)
	{
		std::ifstream Stream(BuildDirectory / L"CMakeFiles" / L"rules.ninja", std::ios::binary);
		std::string   Line;
		while (std::getline(Stream, Line))
		{
			if (Line.find("msvc_deps_prefix") == std::string::npos)
			{
				continue;
			}
			const bool bAscii = std::all_of(Line.begin(), Line.end(), [](char Char) { return static_cast<unsigned char>(Char) < 0x80; });
			return !bAscii && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Line.data(), static_cast<int>(Line.size()), nullptr, 0) > 0;
		}
		return false;
	}

	// CMakeCache.txt의 "KEY:TYPE=값"
	std::string ReadCacheValue(const std::filesystem::path& CacheFile, std::string_view Key)
	{
		std::ifstream Stream(CacheFile);
		std::string   Line;
		while (std::getline(Stream, Line))
		{
			if (Line.size() > Key.size() && Line.compare(0, Key.size(), Key) == 0 && Line[Key.size()] == ':')
			{
				const size_t Equals = Line.find('=');
				return Equals == std::string::npos ? std::string() : Line.substr(Equals + 1);
			}
		}
		return {};
	}
} // namespace

// ---------------------------------------------------------------- 순수 로직

uint32 GameModuleHotReload::FindNextShadowCopyNumber(const std::vector<std::wstring>& ExistingFileNames, const std::string& Name)
{
	const std::wstring Prefix = ToLower(FStringConv::ToWide(Name)) + L"_";
	const std::wstring Suffix = L".dll";
	uint32             Max    = 0;
	for (const std::wstring& FileName : ExistingFileNames)
	{
		const std::wstring Lower = ToLower(FileName);
		if (Lower.size() <= Prefix.size() + Suffix.size() || Lower.compare(0, Prefix.size(), Prefix) != 0 ||
		    Lower.compare(Lower.size() - Suffix.size(), Suffix.size(), Suffix) != 0)
		{
			continue;
		}
		const std::wstring Digits = Lower.substr(Prefix.size(), Lower.size() - Prefix.size() - Suffix.size());
		if (Digits.size() > 9 || !std::all_of(Digits.begin(), Digits.end(), [](wchar_t Char) { return Char >= L'0' && Char <= L'9'; }))
		{
			continue;
		}
		Max = std::max(Max, static_cast<uint32>(std::stoul(Digits)));
	}
	return Max + 1;
}

std::filesystem::path GameModuleHotReload::MakeShadowCopyPath(const std::filesystem::path& Directory, const std::string& Name, uint32 Number)
{
	return Directory / (FStringConv::ToWide(Name) + L"_" + std::to_wstring(Number) + L".dll");
}

bool GameModuleHotReload::ReadPdbPath(const std::vector<uint8>& Image, std::string& OutPath)
{
	size_t Offset = 0, Capacity = 0;
	if (!FindCodeViewPath(Image, Offset, Capacity))
	{
		return false;
	}
	const char* Begin = reinterpret_cast<const char*>(Image.data() + Offset);
	OutPath.assign(Begin, strnlen(Begin, Capacity));
	return true;
}

bool GameModuleHotReload::PatchPdbPath(std::vector<uint8>& Image, const std::string& NewPdbPath)
{
	size_t Offset = 0, Capacity = 0;
	if (!FindCodeViewPath(Image, Offset, Capacity) || NewPdbPath.size() + 1 > Capacity)
	{
		return false;
	}
	std::fill(Image.begin() + static_cast<std::ptrdiff_t>(Offset), Image.begin() + static_cast<std::ptrdiff_t>(Offset + Capacity), uint8(0));
	std::memcpy(Image.data() + Offset, NewPdbPath.data(), NewPdbPath.size());
	return true;
}

void FFileChangeDebouncer::SetBaseline(const FStamp& Stamp)
{
	Baseline = Stamp;
	bPending = false;
}

bool FFileChangeDebouncer::Update(double NowSeconds, const FStamp& Observed, bool bCanConsume)
{
	if (!Observed.bExists)
	{
		return false; // 링커가 지웠다 다시 쓰는 중일 수 있다 — 기다린다
	}
	if (Observed == Baseline)
	{
		bPending = false;
		return false;
	}
	if (!bPending || !(Observed == LastObserved))
	{
		bPending       = true;
		LastObserved   = Observed;
		LastChangeTime = NowSeconds;
		return false;
	}
	if (NowSeconds - LastChangeTime < StableSeconds || !bCanConsume)
	{
		return false;
	}
	Baseline = Observed;
	bPending = false;
	return true;
}

// ---------------------------------------------------------------- 빌드

FGameModuleBuild::~FGameModuleBuild()
{
	Cancel();
	Join();
}

std::filesystem::path FGameModuleBuild::FindBuildDirectory()
{
	wchar_t Buffer[MAX_PATH]{};
	GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
	std::filesystem::path Directory = std::filesystem::path(Buffer).parent_path();
	for (int32 Depth = 0; Depth < 4 && !Directory.empty(); ++Depth)
	{
		std::error_code Error;
		if (std::filesystem::exists(Directory / L"CMakeCache.txt", Error))
		{
			return Directory;
		}
		if (Directory == Directory.parent_path())
		{
			break;
		}
		Directory = Directory.parent_path();
	}
	return {};
}

double FGameModuleBuild::GetElapsedSeconds() const
{
	return bRunning ? NowSeconds() - StartTime : 0.0;
}

bool FGameModuleBuild::Start(const std::string& Target, std::string& OutError)
{
	if (bRunning)
	{
		OutError = "이미 빌드 중입니다";
		return false;
	}
	Join();

	const std::filesystem::path BuildDirectory = FindBuildDirectory();
	if (BuildDirectory.empty())
	{
		OutError = "에디터 실행 파일 위에서 빌드 폴더(CMakeCache.txt)를 찾지 못했습니다";
		return false;
	}
	const std::filesystem::path CacheFile = BuildDirectory / L"CMakeCache.txt";
	const std::filesystem::path CMake     = FStringConv::ToWide(ReadCacheValue(CacheFile, "CMAKE_COMMAND"));
	std::error_code             FsError;
	if (CMake.empty() || !std::filesystem::exists(CMake, FsError))
	{
		OutError = "CMakeCache.txt에서 cmake 경로(CMAKE_COMMAND)를 찾지 못했습니다";
		return false;
	}

	// Ninja + cl은 VS 개발자 환경(INCLUDE/LIB)이 필요하다. 에디터가 이미 그 환경에서 실행됐으면 그대로, 아니면 VsDevCmd.bat
	std::filesystem::path VsDevCmd;
	wchar_t               IncludeBuffer[8]{};
	const bool            bHasDevEnvironment = GetEnvironmentVariableW(L"VCToolsInstallDir", IncludeBuffer, 8) > 0;
	if (!bHasDevEnvironment)
	{
		for (std::filesystem::path Directory = CMake.parent_path(); !Directory.empty() && Directory != Directory.parent_path(); Directory = Directory.parent_path())
		{
			if (std::filesystem::exists(Directory / L"Common7" / L"Tools" / L"VsDevCmd.bat", FsError))
			{
				VsDevCmd = Directory / L"Common7" / L"Tools" / L"VsDevCmd.bat";
				break;
			}
		}
	}

#if E_DEBUG
	const wchar_t* Config = L"Debug";
#else
	const wchar_t* Config = L"Release";
#endif
	std::wstring Inner = L"\"" + CMake.wstring() + L"\" --build \"" + BuildDirectory.wstring() + L"\" --target " + FStringConv::ToWide(Target) +
	                     L" --config " + Config;
	if (!VsDevCmd.empty())
	{
		Inner = L"\"" + VsDevCmd.wstring() + L"\" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1 && " + Inner;
	}
	// Ninja는 cl /showIncludes 줄을 구성 때 기록한 접두사(msvc_deps_prefix) 바이트로 걸러 헤더 의존성을 기록한다.
	// 구성이 UTF-8 콘솔(chcp 65001)에서 됐으면 숨은 콘솔도 UTF-8로 맞춰야 한다 — 다르면 의존성 기록이 빠지고 출력에 섞인다
	if (UsesUtf8DepsPrefix(BuildDirectory))
	{
		Inner = L"chcp 65001 >nul && " + Inner;
	}
	std::wstring CommandLine = L"cmd.exe /d /s /c \"" + Inner + L" 2>&1\"";

	SECURITY_ATTRIBUTES Security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
	HANDLE              ReadPipe = nullptr, WritePipe = nullptr;
	if (!CreatePipe(&ReadPipe, &WritePipe, &Security, 0))
	{
		OutError = std::format("파이프를 만들지 못했습니다 (오류 {})", GetLastError());
		return false;
	}
	SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW Startup{};
	Startup.cb         = sizeof(Startup);
	Startup.dwFlags    = STARTF_USESTDHANDLES;
	Startup.hStdOutput = WritePipe;
	Startup.hStdError  = WritePipe;
	Startup.hStdInput  = nullptr;
	PROCESS_INFORMATION ProcessInfo{};
	const BOOL bCreated = CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
	                                     BuildDirectory.c_str(), &Startup, &ProcessInfo);
	CloseHandle(WritePipe);
	if (!bCreated)
	{
		OutError = std::format("빌드 프로세스를 시작하지 못했습니다 (오류 {})", GetLastError());
		CloseHandle(ReadPipe);
		return false;
	}

	// 잡 객체: 에디터가 끝나거나 취소하면 cmd → cmake → ninja → cl 트리가 함께 끝난다
	HANDLE                               JobHandle = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
	Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (JobHandle != nullptr)
	{
		SetInformationJobObject(JobHandle, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits));
		AssignProcessToJobObject(JobHandle, ProcessInfo.hProcess);
	}
	ResumeThread(ProcessInfo.hThread);
	CloseHandle(ProcessInfo.hThread);

	Job       = JobHandle;
	Process   = ProcessInfo.hProcess;
	StartTime = NowSeconds();
	bRunning  = true;
	{
		std::scoped_lock Lock(Mutex);
		PendingLines.clear();
		Finished.reset();
		PendingLines.push_back(FStringConv::ToUtf8(L"> cmake --build \"" + BuildDirectory.wstring() + L"\" --target " + FStringConv::ToWide(Target) + L" --config " + Config));
	}

	HANDLE ProcessHandle = ProcessInfo.hProcess;
	Worker = std::thread([this, ReadPipe, ProcessHandle]() {
		std::string Pending;
		char        Buffer[4096];
		DWORD       Read = 0;
		while (ReadFile(ReadPipe, Buffer, sizeof(Buffer), &Read, nullptr) && Read > 0)
		{
			Pending.append(Buffer, Read);
			size_t LineEnd = 0;
			while ((LineEnd = Pending.find('\n')) != std::string::npos)
			{
				std::string Line = Pending.substr(0, LineEnd);
				Pending.erase(0, LineEnd + 1);
				if (!Line.empty() && Line.back() == '\r')
				{
					Line.pop_back();
				}
				std::scoped_lock Lock(Mutex);
				PendingLines.push_back(ToUtf8Line(Line));
			}
		}
		CloseHandle(ReadPipe);
		WaitForSingleObject(ProcessHandle, INFINITE);
		DWORD ExitCode = 1;
		GetExitCodeProcess(ProcessHandle, &ExitCode);
		{
			std::scoped_lock Lock(Mutex);
			if (!Pending.empty())
			{
				PendingLines.push_back(ToUtf8Line(Pending));
			}
			Finished = ExitCode == 0;
		}
		bRunning = false;
	});
	return true;
}

void FGameModuleBuild::Poll(std::vector<std::string>& OutLines, std::optional<bool>& OutFinished)
{
	{
		std::scoped_lock Lock(Mutex);
		OutLines.insert(OutLines.end(), std::make_move_iterator(PendingLines.begin()), std::make_move_iterator(PendingLines.end()));
		PendingLines.clear();
		OutFinished = Finished;
		Finished.reset();
	}
	if (OutFinished.has_value())
	{
		Join();
	}
}

void FGameModuleBuild::Cancel()
{
	if (bRunning && Job != nullptr)
	{
		TerminateJobObject(static_cast<HANDLE>(Job), 1);
	}
}

void FGameModuleBuild::Join()
{
	if (Worker.joinable())
	{
		Worker.join();
	}
	if (Process != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(Process));
		Process = nullptr;
	}
	if (Job != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(Job));
		Job = nullptr;
	}
}

// ---------------------------------------------------------------- 핫 리로드 상태

void FGameModuleHotReload::Init(const std::string& InModuleName, const std::filesystem::path& InSourceDll, const std::filesystem::path& InShadowDirectory)
{
	ModuleName      = InModuleName;
	SourceDll       = InSourceDll;
	ShadowDirectory = InShadowDirectory;

	// 이전 실행이 남긴 복사본 정리 (다른 에디터가 아직 쓰는 파일은 지워지지 않으므로 번호를 그 뒤로)
	std::vector<std::wstring> Remaining;
	std::error_code           Error;
	const std::wstring        Prefix = ToLower(FStringConv::ToWide(ModuleName)) + L"_";
	uint32                    Removed = 0;
	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(ShadowDirectory, Error))
	{
		const std::wstring FileName = Entry.path().filename().wstring();
		const std::wstring Lower    = ToLower(FileName);
		const std::wstring Extension = ToLower(Entry.path().extension().wstring());
		if (Lower.compare(0, Prefix.size(), Prefix) != 0 || (Extension != L".dll" && Extension != L".pdb"))
		{
			continue;
		}
		std::error_code RemoveError;
		if (std::filesystem::remove(Entry.path(), RemoveError))
		{
			++Removed;
		}
		else
		{
			Remaining.push_back(FileName);
		}
	}
	NextNumber = GameModuleHotReload::FindNextShadowCopyNumber(Remaining, ModuleName);
	if (Removed > 0 || !Remaining.empty())
	{
		E_LOG(LogEditor, Log, "핫 리로드 복사본 정리: {}개 삭제, 사용 중 {}개", Removed, Remaining.size());
	}
	ResetSourceBaseline();
}

std::filesystem::path FGameModuleHotReload::MakeShadowCopy(std::string& OutError)
{
	std::error_code Error;
	std::filesystem::create_directories(ShadowDirectory, Error);

	std::vector<uint8> Image;
	{
		std::ifstream Stream(SourceDll, std::ios::binary);
		if (!Stream)
		{
			OutError = "게임 모듈 DLL을 읽지 못했습니다: " + FStringConv::ToUtf8(SourceDll.wstring());
			return {};
		}
		Image.assign(std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>());
	}

	std::filesystem::path Copy = GameModuleHotReload::MakeShadowCopyPath(ShadowDirectory, ModuleName, NextNumber++);
	while (std::filesystem::exists(Copy, Error))
	{
		Copy = GameModuleHotReload::MakeShadowCopyPath(ShadowDirectory, ModuleName, NextNumber++);
	}

	// PDB: DLL에 적힌 경로(없거나 상대면 DLL 옆 같은 이름) → 복사본 옆 복사본 이름. DLL의 PDB 경로를 복사본 파일 이름으로 바꾼다
	std::string           EmbeddedPdb;
	std::filesystem::path SourcePdb;
	if (GameModuleHotReload::ReadPdbPath(Image, EmbeddedPdb) && !EmbeddedPdb.empty())
	{
		const std::filesystem::path Embedded = FStringConv::ToWide(EmbeddedPdb);
		SourcePdb = Embedded.is_absolute() ? Embedded : SourceDll.parent_path() / Embedded.filename();
	}
	if (SourcePdb.empty() || !std::filesystem::exists(SourcePdb, Error))
	{
		SourcePdb = std::filesystem::path(SourceDll).replace_extension(L".pdb");
	}
	const std::filesystem::path CopyPdb = std::filesystem::path(Copy).replace_extension(L".pdb");
	if (std::filesystem::exists(SourcePdb, Error))
	{
		std::error_code CopyError;
		std::filesystem::copy_file(SourcePdb, CopyPdb, std::filesystem::copy_options::overwrite_existing, CopyError);
		if (CopyError)
		{
			E_LOG(LogEditor, Warning, "게임 모듈 PDB 복사 실패 (디버그 심볼 없이 계속): {}", CopyError.message());
		}
		else if (!GameModuleHotReload::PatchPdbPath(Image, FStringConv::ToUtf8(CopyPdb.filename().wstring())))
		{
			E_LOG(LogEditor, Warning, "게임 모듈 DLL의 PDB 경로를 바꾸지 못했습니다 — 디버거가 원본 PDB를 잡을 수 있습니다");
		}
	}

	{
		std::ofstream Stream(Copy, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Image.data()), static_cast<std::streamsize>(Image.size()));
		if (!Stream)
		{
			OutError = "게임 모듈 복사본을 쓰지 못했습니다: " + FStringConv::ToUtf8(Copy.wstring());
			return {};
		}
	}
	return Copy;
}

FFileChangeDebouncer::FStamp FGameModuleHotReload::ReadSourceStamp() const
{
	FFileChangeDebouncer::FStamp Stamp;
	std::error_code              Error;
	const auto                   WriteTime = std::filesystem::last_write_time(SourceDll, Error);
	if (Error)
	{
		return Stamp;
	}
	const uint64 Size = std::filesystem::file_size(SourceDll, Error);
	if (Error)
	{
		return Stamp;
	}
	Stamp.WriteTime = WriteTime.time_since_epoch().count();
	Stamp.Size      = Size;
	Stamp.bExists   = true;
	return Stamp;
}

bool FGameModuleHotReload::IsSourceWritable() const
{
	// 링커가 아직 쓰고 있으면(쓰기 핸들) 쓰기 거부 공유로 열 수 없다
	FILE* File = _wfsopen(SourceDll.c_str(), L"rb", _SH_DENYWR);
	if (File == nullptr)
	{
		return false;
	}
	std::fclose(File);
	return true;
}

bool FGameModuleHotReload::PollSourceChange(double Now)
{
	if (!IsConfigured() || (LastPollTime >= 0.0 && Now - LastPollTime < 0.5))
	{
		return false;
	}
	LastPollTime = Now;
	const FFileChangeDebouncer::FStamp Stamp = ReadSourceStamp();
	return Debouncer.Update(Now, Stamp, !Debouncer.IsPending() || IsSourceWritable());
}

void FGameModuleHotReload::ResetSourceBaseline()
{
	Debouncer.SetBaseline(ReadSourceStamp());
}

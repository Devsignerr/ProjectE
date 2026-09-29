#include "Core/FileWatcher.h"

#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <cwctype>

namespace
{
	std::wstring MakeKey(const std::filesystem::path& Path)
	{
		std::wstring Key = Path.lexically_normal().wstring();
		std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Key;
	}

	constexpr DWORD GNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
	constexpr size_t GBufferDwords = 16 * 1024; // 64KB (DWORD 정렬 필수)
} // namespace

// ---------------------------------------------------------------- FChangeDebouncer

void FChangeDebouncer::AddEvent(const std::filesystem::path& Path, FTimePoint Time)
{
	const std::wstring Key   = MakeKey(Path);
	auto               Found = Pending.find(Key);
	if (Found != Pending.end())
	{
		Found->second.LastTime = Time;
		return;
	}
	Pending.emplace(Key, FPendingEvent{ Path, Time, NextOrder++ });
}

std::vector<std::filesystem::path> FChangeDebouncer::CollectReady(FTimePoint Now)
{
	std::vector<FPendingEvent> Ready;
	for (auto Iterator = Pending.begin(); Iterator != Pending.end();)
	{
		if (Now - Iterator->second.LastTime >= Delay)
		{
			Ready.push_back(std::move(Iterator->second));
			Iterator = Pending.erase(Iterator);
		}
		else
		{
			++Iterator;
		}
	}

	std::sort(Ready.begin(), Ready.end(), [](const FPendingEvent& A, const FPendingEvent& B) { return A.Order < B.Order; });

	std::vector<std::filesystem::path> Result;
	Result.reserve(Ready.size());
	for (FPendingEvent& Event : Ready)
	{
		Result.push_back(std::move(Event.Path));
	}
	return Result;
}

// ---------------------------------------------------------------- FFileWatcher

struct FFileWatcher::FImpl
{
	HANDLE             DirectoryHandle = INVALID_HANDLE_VALUE;
	OVERLAPPED         Overlapped{};
	std::vector<DWORD> Buffer = std::vector<DWORD>(GBufferDwords);
	bool               bReadPending = false;
};

FFileWatcher::FFileWatcher()
	: Impl(std::make_unique<FImpl>())
{
}

FFileWatcher::~FFileWatcher()
{
	Stop();
}

bool FFileWatcher::IsWatching() const
{
	return Impl->DirectoryHandle != INVALID_HANDLE_VALUE;
}

bool FFileWatcher::Start(const std::filesystem::path& InDirectory, bool bInRecursive, std::chrono::milliseconds DebounceDelay)
{
	Stop();

	std::error_code ErrorCode;
	Directory  = std::filesystem::weakly_canonical(InDirectory, ErrorCode);
	if (ErrorCode)
	{
		Directory = InDirectory;
	}
	bRecursive = bInRecursive;
	Debouncer  = FChangeDebouncer(DebounceDelay);

	Impl->DirectoryHandle = CreateFileW(Directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	if (Impl->DirectoryHandle == INVALID_HANDLE_VALUE)
	{
		E_LOG(LogCore, Error, "디렉터리 감시 시작 실패: {} (오류 코드 {})", FStringConv::ToUtf8(Directory.wstring()), GetLastError());
		return false;
	}

	Impl->Overlapped        = OVERLAPPED{};
	Impl->Overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (Impl->Overlapped.hEvent == nullptr || !IssueRead())
	{
		Stop();
		return false;
	}

	E_LOG(LogCore, Log, "디렉터리 감시 시작: {}", FStringConv::ToUtf8(Directory.wstring()));
	return true;
}

void FFileWatcher::Stop()
{
	if (Impl->DirectoryHandle != INVALID_HANDLE_VALUE)
	{
		if (Impl->bReadPending)
		{
			CancelIoEx(Impl->DirectoryHandle, &Impl->Overlapped);
			DWORD Ignored = 0;
			GetOverlappedResult(Impl->DirectoryHandle, &Impl->Overlapped, &Ignored, TRUE); // 취소 완료 대기 (버퍼 해제 전 필수)
			Impl->bReadPending = false;
		}
		CloseHandle(Impl->DirectoryHandle);
		Impl->DirectoryHandle = INVALID_HANDLE_VALUE;
	}
	if (Impl->Overlapped.hEvent != nullptr)
	{
		CloseHandle(Impl->Overlapped.hEvent);
		Impl->Overlapped.hEvent = nullptr;
	}
	Debouncer.Clear();
}

bool FFileWatcher::IssueRead()
{
	ResetEvent(Impl->Overlapped.hEvent);
	const BOOL bOk = ReadDirectoryChangesW(Impl->DirectoryHandle, Impl->Buffer.data(), static_cast<DWORD>(Impl->Buffer.size() * sizeof(DWORD)),
	                                       bRecursive ? TRUE : FALSE, GNotifyFilter, nullptr, &Impl->Overlapped, nullptr);
	if (!bOk)
	{
		E_LOG(LogCore, Error, "ReadDirectoryChangesW 실패 (오류 코드 {})", GetLastError());
		Impl->bReadPending = false;
		return false;
	}
	Impl->bReadPending = true;
	return true;
}

void FFileWatcher::ParseNotifications(uint32 BytesTransferred)
{
	const FChangeDebouncer::FTimePoint Now = FChangeDebouncer::FClock::now();

	if (BytesTransferred == 0)
	{
		// 버퍼 오버플로: 개별 변경을 알 수 없다. 감시 디렉터리 자체를 보고하면 호출자가 전체 갱신할 수 있으나,
		// 여기서는 경고만 남긴다 (셰이더 디렉터리 규모에서는 발생하지 않음)
		E_LOG(LogCore, Warning, "파일 변경 알림 버퍼 오버플로: {}", FStringConv::ToUtf8(Directory.wstring()));
		return;
	}

	const uint8* Cursor = reinterpret_cast<const uint8*>(Impl->Buffer.data());
	for (;;)
	{
		const FILE_NOTIFY_INFORMATION* Info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(Cursor);
		const std::wstring             Name(Info->FileName, Info->FileNameLength / sizeof(wchar_t));

		switch (Info->Action)
		{
		case FILE_ACTION_ADDED:
		case FILE_ACTION_MODIFIED:
		case FILE_ACTION_RENAMED_NEW_NAME:
			Debouncer.AddEvent(Directory / Name, Now);
			break;
		default: // 삭제, 이름 변경 전 이름은 무시
			break;
		}

		if (Info->NextEntryOffset == 0)
		{
			break;
		}
		Cursor += Info->NextEntryOffset;
	}
}

std::vector<std::filesystem::path> FFileWatcher::Poll()
{
	if (!IsWatching())
	{
		return {};
	}

	// 완료된 읽기를 모두 처리 (논블로킹)
	while (Impl->bReadPending)
	{
		DWORD Bytes = 0;
		if (!GetOverlappedResult(Impl->DirectoryHandle, &Impl->Overlapped, &Bytes, FALSE))
		{
			const DWORD Error = GetLastError();
			if (Error == ERROR_IO_INCOMPLETE)
			{
				break;
			}
			E_LOG(LogCore, Warning, "디렉터리 감시 읽기 실패 (오류 코드 {}), 재시작", Error);
			Impl->bReadPending = false;
			IssueRead();
			break;
		}

		Impl->bReadPending = false;
		ParseNotifications(static_cast<uint32>(Bytes));
		if (!IssueRead())
		{
			break;
		}
	}

	// 디바운스가 끝난 것 중 현재 존재하는 일반 파일만 보고
	std::vector<std::filesystem::path> Result;
	for (std::filesystem::path& Path : Debouncer.CollectReady(FChangeDebouncer::FClock::now()))
	{
		std::error_code ErrorCode;
		if (std::filesystem::is_regular_file(Path, ErrorCode))
		{
			Result.push_back(std::move(Path));
		}
	}
	return Result;
}

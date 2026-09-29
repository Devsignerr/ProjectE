#pragma once

#include "Core/CoreTypes.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// 파일 변경 이벤트 디바운서 (순수 로직, 테스트 가능).
// 같은 경로의 이벤트가 연속으로 오면 마지막 이벤트 후 Delay가 지났을 때 한 번만 보고한다.
class FChangeDebouncer
{
public:
	using FClock     = std::chrono::steady_clock;
	using FTimePoint = FClock::time_point;

	explicit FChangeDebouncer(std::chrono::milliseconds InDelay = std::chrono::milliseconds(150))
		: Delay(InDelay)
	{
	}

	void AddEvent(const std::filesystem::path& Path, FTimePoint Time);

	// Delay가 지난 경로를 반환하고 대기 목록에서 제거 (추가된 순서 유지)
	std::vector<std::filesystem::path> CollectReady(FTimePoint Now);

	size_t GetPendingCount() const { return Pending.size(); }
	void   Clear() { Pending.clear(); }

private:
	struct FPendingEvent
	{
		std::filesystem::path Path;
		FTimePoint            LastTime;
		uint64                Order = 0;
	};

	std::chrono::milliseconds                      Delay;
	std::unordered_map<std::wstring, FPendingEvent> Pending; // 키: 정규화된 소문자 경로
	uint64                                         NextOrder = 0;
};

// 디렉터리 변경 감시 (Win32 ReadDirectoryChangesW, overlapped I/O).
// 별도 스레드 없이 메인 스레드에서 Poll()을 매 프레임 호출하는 방식이다 (블로킹 없음).
// 보고 대상: 파일 생성/수정/이름 변경(새 이름). 삭제와 디렉터리는 제외.
// "임시 파일에 쓰고 이름 바꾸기" 저장 방식도 최종 파일명으로 보고된다 (Poll 시점에 없는 파일은 제외).
class FFileWatcher
{
public:
	FFileWatcher();
	~FFileWatcher();

	FFileWatcher(const FFileWatcher&)            = delete;
	FFileWatcher& operator=(const FFileWatcher&) = delete;

	bool Start(const std::filesystem::path& Directory, bool bRecursive = true,
	           std::chrono::milliseconds DebounceDelay = std::chrono::milliseconds(150));
	void Stop();

	bool                         IsWatching() const;
	const std::filesystem::path& GetDirectory() const { return Directory; }

	// 디바운스가 끝난 변경 파일의 절대 경로 목록 (없으면 빈 목록)
	std::vector<std::filesystem::path> Poll();

private:
	struct FImpl;

	bool IssueRead();
	void ParseNotifications(uint32 BytesTransferred);

	std::unique_ptr<FImpl> Impl;
	std::filesystem::path  Directory;
	bool                   bRecursive = true;
	FChangeDebouncer       Debouncer;
};

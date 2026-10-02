#pragma once

#include "Core/CoreTypes.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// 작은 작업자 스레드 풀 + 메인 스레드 완료 대기열.
//   - Submit(작업, 완료): 작업은 작업자 스레드에서, 완료 콜백은 메인 스레드가 PumpCompletions를 부를 때 (제출 순서가 아니라 끝난 순서) 실행된다
//   - 작업에서는 파일 읽기/디코딩/압축/파싱만 한다. ECS·GPU·리소스 관리자 접근은 완료 콜백(메인 스레드)에서
//   - WorkerCount 0이면 스레드 없이 Submit 안에서 바로 실행한다 (완료 콜백은 여전히 PumpCompletions에서)
//   - Shutdown: 아직 시작하지 않은 작업과 완료 콜백은 버리고, 실행 중인 작업이 끝나기를 기다린다
class FJobQueue
{
public:
	using FWork       = std::function<void()>;
	using FCompletion = std::function<void()>;

	FJobQueue() = default;
	~FJobQueue() { Shutdown(); }
	FJobQueue(const FJobQueue&)            = delete;
	FJobQueue& operator=(const FJobQueue&) = delete;

	void Init(uint32 WorkerCount, const char* ThreadName = "작업자");
	void Shutdown();

	void Submit(FWork Work, FCompletion Completion = {});

	// 끝난 작업의 완료 콜백을 실행 (메인 스레드). 실행한 수를 반환
	uint32 PumpCompletions();

	// 대기 중·실행 중 작업이 모두 끝날 때까지 기다린다 (완료 콜백은 실행하지 않음)
	void WaitIdle();

	// 아직 완료 콜백이 실행되지 않은 작업 수 (대기 + 실행 중 + 완료 대기열)
	uint32 GetOutstandingCount() const;
	uint32 GetWorkerCount() const { return static_cast<uint32>(Workers.size()); }

	// 기본 작업자 수: 논리 코어 - 2 (메인/렌더 몫), 1~4
	static uint32 GetDefaultWorkerCount();

private:
	struct FJob
	{
		FWork       Work;
		FCompletion Completion;
	};

	void WorkerMain();

	mutable std::mutex       Mutex;
	std::condition_variable  WorkAvailable;
	std::condition_variable  WorkDone;
	std::deque<FJob>         Pending;
	std::vector<FCompletion> Completed;
	std::vector<std::thread> Workers;
	uint32                   Running     = 0;
	uint32                   Outstanding = 0;
	bool                     bStopping   = false;
	bool                     bInitialized = false;
};

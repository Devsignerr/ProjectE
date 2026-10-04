#pragma once

#include "Core/CoreTypes.h"
#include "Core/FrameTime.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// 렌더 스레드 (r.RenderThread, 기본 0 — 이 파일 머리 주석이 규칙 기준)
//
// 목표: 게임 스레드가 프레임 N+1을 갱신하는 동안 렌더 스레드가 프레임 N의 GPU 명령을 기록·제출해 프레임 시간을
//   합(게임 + 렌더)이 아니라 max(게임 + 준비, 기록 + 제출)에 가깝게 한다 (언리얼 게임/렌더 스레드의 1단계).
//
// 1단계 경계 (지금): "씬을 읽는 것은 게임 스레드, 명령 기록만 렌더 스레드".
//   게임 스레드 (OnRender 앞부분): 직전 렌더 작업 대기(WaitIdle) → RHI BeginFrame(펜스 대기·할당자/동적 버퍼 재설정·비동기 업로드 완료·
//     스트리밍) → FSceneRenderer::BeginRender = 씬을 읽는 모든 CPU 준비(수집·스킨 팔레트·인스턴스/묶음·LOD·그림자 묶음·RT Prepare·
//     파티클 수집과 입자 업로드·동적 버퍼 업로드)와 렌더 그래프 패스 등록 → 오버레이 입력 사본(디버그 선 목록, UI 그리기 목록 +
//     UI 텍스처 준비, 카메라·출력·스크린샷 경로) → Kick.
//   렌더 스레드: FSceneRenderer::FinishRender(그래프 컴파일·실행 = 람다 기록, 통계) → 디버그 선·UI 기록 → RHI EndFrame(제출 + Present,
//     스크린샷 리드백). 그동안 게임 스레드는 다음 프레임 OnUpdate(입력·스크립트·물리·애니메이션·트랜스폼)를 돈다.
//   즉 "추출된 프레임 데이터"(언리얼 렌더 프록시에 해당)는 등록이 끝난 렌더 그래프 자체다: 패스 람다는 값 캡처 + 렌더러 소유 상태만 읽고
//   씬(ECS)·게임 전역(FDebugDraw, UI 글꼴 라이브러리, 앱 카메라)은 읽지 않는다. CPU에서 동시에 도는 프레임은 하나(이중 버퍼 불필요):
//   게임 스레드는 렌더 상태(렌더러·RHI 프레임 상태·패킷)를 WaitIdle 뒤에만 쓰고, 렌더 스레드는 Kick~작업 끝 사이에만 읽는다.
//   GPU 쪽 프레임 겹침(백버퍼 슬롯 펜스)은 예전과 같다.
//
// 스레드별로 만질 수 있는 것:
//   렌더 스레드 = 명령 목록 기록, 렌더러 소유 상태(묶음·인스턴스 목록·프레임 그래프 풀·통계), 리소스 관리자 읽기(Get/Resolve — 풀 포인터),
//     동적 업로드 버퍼(이 프레임 슬롯), PSO 캐시(지연 생성 — 원래 스레드 안전), RHI DeferRelease(잠금), 로그.
//     금지: 씬/ECS 읽기·쓰기, 리소스 관리자 변경(Load/Create/Destroy/Apply…), 콘솔 변수 쓰기, 게임 전역 상태.
//   게임 스레드 = 나머지 전부. 단 렌더 스레드와 공유하는 상태를 **바꾸기 직전** RenderThreadSync::WaitForRenderThread()
//     (Core/RenderThreadSync.h, 언리얼 FlushRenderingCommands): 리소스 관리자 변경 함수 전부(경로 캐시 적중은 대기 없음),
//     콘솔 변수 값 변경(같은 값이면 대기 없음 — 기록 코드가 CVar를 읽고, 변경 콜백이 렌더 리소스를 다시 만든다).
//     앱이 직접 기다리는 곳: 맵 전환, 창 크기 변경/모드·VSync 변경, 열린 콘솔 입력, 종료, HDR 출력 전환(BeginFrame 전, WaitIdle 뒤).
//   기다림이 생긴 프레임은 겹침이 없을 뿐 결과는 같다 (로드가 많은 프레임 = 예전처럼 순차).
//   GPU 리소스를 소유한 컴포넌트(GPU 파티클 풀)는 shared_ptr로 그 프레임 람다가 붙잡는다 (게임 스레드가 이미터를 지워도 기록이 끝날 때까지 산다).
//   FParallel::ParallelFor는 두 스레드가 동시에 부르면 늦게 온 쪽이 순차로 돈다 (작업 칸 하나 — 결과는 같다).
//
// 결정성: 기록되는 명령은 r.RenderThread 0/1이 같다 (같은 함수를 같은 순서로 — 0이면 Kick이 그 자리에서 실행).
//   기록 중 읽는 FFrameTime(머티리얼 Time, 노출 적응 등)은 Kick이 넘긴 그 프레임 값(FFrameTime::FScopedOverride) → --fixed-delta 화면 비트 동일.
//
// 에디터는 쓰지 않는다 (ImGui 기록·편집기 패널·미리보기 렌더러가 씬과 렌더러를 한 프레임 안에서 오가므로 단일 스레드 유지).
// 셰이더 핫 리로드(에디터)·반사 캡처 굽기는 게임 스레드 BeginRender 안이라 그대로 동작한다.
//
// 남은 일(2단계): 수집(인스턴스·팔레트 — 프레임 CPU의 큰 몫)을 렌더 스레드로 옮기려면 게임 스레드가 프레임 끝에 트랜스폼·메시·스킨 뼈
//   월드 행렬을 불변 사본(프록시)으로 추출해야 한다 — 지금은 수집 자체가 그 추출이라 게임 스레드에 있다.
class FRenderThread
{
public:
	FRenderThread() = default;
	~FRenderThread();
	FRenderThread(const FRenderThread&)            = delete;
	FRenderThread& operator=(const FRenderThread&) = delete;

	// 작업 스레드를 만들고 RenderThreadSync 대기 처리기를 등록한다 (이미 돌면 아무것도 안 함). 동시에 하나만 등록된다
	void Start();
	// 진행 중 작업을 기다리고 스레드를 끝낸다 (처리기 해제)
	void Stop();
	bool IsRunning() const { return Thread.joinable(); }

	// 게임 스레드: 작업 하나를 넘긴다. 이전 작업이 끝나 있어야 한다 (먼저 WaitIdle).
	//   스레드가 없으면(bThreaded = false 또는 Start 전) 호출한 스레드에서 바로 실행한다 — r.RenderThread 0 경로와 같은 함수
	//   Time: 작업 안에서 FFrameTime이 돌려줄 값 (보통 FFrameTime::Capture())
	void Kick(std::function<void()> Work, const FFrameTime::FSnapshot& Time, bool bThreaded);
	// 게임 스레드: 진행 중 작업이 끝날 때까지 (없으면 바로). 렌더 스레드에서 부르면 아무것도 안 함
	void WaitIdle();
	bool IsBusy() const { return bBusy.load(std::memory_order_acquire); }
	// 현재 스레드가 이 렌더 스레드인가
	bool IsCurrentThread() const { return std::this_thread::get_id() == ThreadId.load(std::memory_order_acquire); }

	// 측정: 렌더 스레드 작업 시간 합, 게임 스레드가 WaitIdle로 막힌 시간 합 (ms), 작업 수
	struct FStats
	{
		double WorkMs  = 0.0;
		double WaitMs  = 0.0;
		uint64 Frames  = 0;
		uint64 Threaded = 0; // 그중 렌더 스레드에서 돈 작업 수
	};
	FStats GetStats() const;
	void   ResetStats();

private:
	void ThreadMain();
	void RunWork(std::function<void()>& Work, const FFrameTime::FSnapshot& Time);

	std::thread                  Thread;
	std::atomic<std::thread::id> ThreadId{};
	mutable std::mutex           Mutex;
	std::condition_variable      WakeWorker;
	std::condition_variable      WorkDone;
	std::function<void()>        PendingWork;
	FFrameTime::FSnapshot        PendingTime;
	bool                         bHasWork  = false;
	bool                         bStopping = false;
	std::atomic<bool>            bBusy{ false };
	FStats                       Stats;
};

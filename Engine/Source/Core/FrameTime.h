#pragma once

#include "Core/CoreTypes.h"

// 앱 프레임 시간 (엔진 DLL 전역 하나).
//   화면·시뮬레이션 상태(자동 노출 적응, 물·구름·머티리얼 Time, 쿠키 패닝, 스트리밍 내림 지연 등)는
//   실제 시각(steady_clock)이 아니라 이 시간으로 진행한다 — `--fixed-delta`면 실행마다(부하와 무관하게) 같은 화면이 된다.
//   FApplication이 프레임(헤드리스는 틱)마다 FTimer 델타로 Advance한다. 앱 루프 밖(테스트·도구)은 0에서 멈춰 있다.
//   측정(프레임 간격, CPU/GPU 구간, 로딩 시간, 업로드 속도)은 계속 실제 시각을 쓴다
class FFrameTime
{
public:
	// 한 프레임 진행 (FApplication 전용)
	static void Advance(float DeltaSeconds);
	// 0으로 되돌린다 (앱 시작)
	static void Reset();

	// 이번 프레임 델타 (초)
	static float GetDeltaSeconds();
	// 누적 시간 (초, 델타의 합 — 디버거 정지 등으로 버린 시간은 들어가지 않는다)
	static double GetTotalSeconds();
	// Advance 횟수
	static uint64 GetFrameCount();

	// 한 프레임의 시간 값 (렌더 스레드가 기록하는 프레임의 시간 — Renderer/RenderThread.h)
	struct FSnapshot
	{
		float  DeltaSeconds = 0.0f;
		double TotalSeconds = 0.0;
		uint64 FrameCount   = 0;
	};
	// 현재 값 (게임 스레드)
	static FSnapshot Capture();

	// 이 스레드에서만 Get*이 Snapshot을 돌려주게 한다 (범위 동안). 렌더 스레드가 프레임 N을 기록하는 동안 게임 스레드는 이미
	// N+1로 Advance했으므로, 기록 중 읽는 머티리얼 Time·노출 적응 등이 r.RenderThread 0과 같은 값을 보게 한다
	class FScopedOverride
	{
	public:
		explicit FScopedOverride(const FSnapshot& Snapshot);
		~FScopedOverride();
		FScopedOverride(const FScopedOverride&)            = delete;
		FScopedOverride& operator=(const FScopedOverride&) = delete;

	private:
		const FSnapshot* Previous = nullptr;
		FSnapshot        Value;
	};
};

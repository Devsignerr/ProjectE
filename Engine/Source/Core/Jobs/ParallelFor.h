#pragma once

#include "Core/CoreTypes.h"

#include <functional>

// 프레임 안 데이터 병렬 루프 (애니메이션 포즈, 스킨 팔레트 등 엔티티마다 독립인 대량 계산용).
//   - ParallelFor(Count, MinBatch, Body): [0, Count)를 묶음으로 나눠 작업자 스레드 + 호출 스레드가 함께 처리하고, 모두 끝나야 돌아온다
//   - Body(Begin, End)는 서로 겹치지 않는 구간을 받는다. 구간끼리 같은 데이터를 쓰지 않아야 하며(결과는 실행 순서와 무관 = 결정적),
//     ECS 구조 변경(엔티티/컴포넌트 추가·제거)·GPU·리소스 관리자·로그 외 전역 상태 변경은 하지 않는다
//   - 중첩 호출(본문 안의 ParallelFor)은 그 자리에서 순차 실행한다. Count < 2 * MinBatch거나 작업자가 없으면 호출 스레드에서 순차 실행
//   - 작업자 풀은 엔진 DLL 전역 하나(처음 쓸 때 생성, 논리 코어 - 1, 최대 15). FJobQueue(파일 IO 작업)와는 별개
//   - 콘솔 변수 core.ParallelFor 0 = 항상 순차 (비교·디버깅)
namespace FParallel
{
	using FBody = std::function<void(uint32 Begin, uint32 End)>;

	void   ParallelFor(uint32 Count, uint32 MinBatch, const FBody& Body);
	uint32 GetWorkerCount();
	// 프로세스 종료 전에 작업자를 멈춘다 (엔진 종료 경로가 부른다 — 안 불러도 정적 소멸 때 정리)
	void Shutdown();
} // namespace FParallel

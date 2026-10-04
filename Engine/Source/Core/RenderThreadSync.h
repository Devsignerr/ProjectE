#pragma once

#include "Core/CoreTypes.h"

// 게임 스레드 → 렌더 스레드 동기화 지점 (Renderer/RenderThread.h 머리 주석이 규칙 기준).
//   렌더 스레드가 프레임을 기록하는 동안 게임 스레드가 렌더 스레드와 공유하는 상태(리소스 관리자 풀·GPU 리소스 생성/해제,
//   콘솔 변수 값)를 바꾸려는 곳은 바꾸기 직전에 WaitForRenderThread()를 부른다 → 진행 중 프레임 기록이 끝날 때까지 기다린다
//   (언리얼 FlushRenderingCommands). 렌더 스레드가 없거나 쉬고 있으면 바로 돌아오고(원자 변수 하나 확인),
//   렌더 스레드 자신이 부르면 아무것도 하지 않는다. Core에 두는 이유: 콘솔 변수(Core)와 리소스 관리자(Renderer)가 함께 쓴다
namespace RenderThreadSync
{
	using FWaitHandler = void (*)();

	// 렌더 스레드 구현이 등록한다 (nullptr = 해제). 엔진 DLL 전역 하나
	void SetWaitHandler(FWaitHandler Handler);
	// 진행 중인 렌더 스레드 작업이 끝날 때까지 기다린다 (없으면 바로 반환)
	void WaitForRenderThread();
} // namespace RenderThreadSync

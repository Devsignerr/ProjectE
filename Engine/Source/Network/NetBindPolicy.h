#pragma once

// 서버 대기·LAN 응답 소켓을 어느 주소에 열지 정한다.
// Windows 방화벽 확인 창("개인/공용 네트워크에서 허용")은 모든 주소(0.0.0.0 / ::)에서 대기할 때 exe 경로마다 뜨고,
// 루프백(127.0.0.1)에서만 대기하면 뜨지 않는다.
//   루프백 전용 = 같은 PC 안의 접속만: 자동 검증(--exit-after) 실행, 에디터 네트워크 플레이와 그 자식 프로세스
//   공개 = 실제 LAN/인터넷: --host, 사람이 실행한 전용 서버 (처음 한 번 확인 창은 정상)
// 명령줄 --net-local(루프백 강제) / --net-public(공개 강제)이 앱의 기본값보다 우선한다
namespace NetBindPolicy
{
	// 앱이 세션을 열기 전에 부른다 (기본값 = 이 앱 실행이 같은 PC 전용인가)
	void Configure(bool bDefaultLoopbackOnly);
	bool IsLoopbackOnly();
}

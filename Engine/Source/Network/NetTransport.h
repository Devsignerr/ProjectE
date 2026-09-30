#pragma once

#include "Network/NetTypes.h"

#include <memory>
#include <string>
#include <vector>

// 메시지 단위 전송 계층 추상화. 구현: GameNetworkingSockets(CreateGnsTransport), 테스트용 루프백(FLoopbackTransport).
// 모든 함수는 메인 스레드에서 호출한다. 이벤트는 Poll에서만 나온다 (콜백 없음)
class INetTransport
{
public:
	virtual ~INetTransport() = default;

	// 서버: 모든 주소의 Port에서 접속 대기. GNS는 포트 0(자동 할당)을 받지 않는다
	virtual bool Listen(uint16 Port) = 0;
	// 클라이언트: "ip:port"로 접속 시작. 실패하면 InvalidNetConnection. 수립/실패는 이후 Poll 이벤트로 알린다
	virtual FNetConnectionId Connect(const std::string& Address) = 0;

	virtual bool Send(FNetConnectionId Connection, const void* Data, uint32 Size, ENetReliability Reliability) = 0;
	// 연결을 닫는다 (상대에게는 Disconnected). 자신에게는 이벤트가 나오지 않는다
	virtual void Disconnect(FNetConnectionId Connection, const std::string& Reason) = 0;

	// 쌓인 이벤트를 OutEvents 뒤에 붙인다 (매 틱)
	virtual void Poll(std::vector<FNetEvent>& OutEvents) = 0;
	// 모든 연결과 대기를 닫는다
	virtual void Close() = 0;

	// 디버그: 연결 상태 (지원하지 않거나 없는 연결이면 false)
	virtual bool GetStats(FNetConnectionId /*Connection*/, FNetConnectionStats& /*OutStats*/) const { return false; }
	// 디버그: 보내는 패킷에 지연/손실을 흉내 낸다 (GNS: 프로세스 전역 설정). 0이면 끔
	virtual void SetSimulation(int32 /*LatencyMs*/, float /*LossPercent*/) {}
};

// GameNetworkingSockets 전송 계층. 프로세스 안에 여러 개를 만들 수 있다 (서버+클라이언트 동시 실행 가능)
std::unique_ptr<INetTransport> CreateGnsTransport();

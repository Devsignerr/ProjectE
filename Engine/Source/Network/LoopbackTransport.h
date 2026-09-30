#pragma once

#include "Network/NetTransport.h"

#include <memory>
#include <unordered_map>

class FLoopbackTransport;

// 루프백 전송 계층들이 공유하는 가상 네트워크 (포트 → 대기 중인 전송 계층). 소켓 없이 같은 프로세스 안에서만 동작
class FLoopbackHub
{
public:
	std::unordered_map<uint16, FLoopbackTransport*> Listeners;
	FNetConnectionId                                NextConnection = 1;
};

// 메모리 전송 계층 (테스트/프로세스 내부용). 메시지는 손실·재정렬 없이 상대의 다음 Poll에 도착한다.
// 주소는 "아무거나:포트" 형식이며 포트만 본다
class FLoopbackTransport final : public INetTransport
{
public:
	explicit FLoopbackTransport(std::shared_ptr<FLoopbackHub> InHub);
	~FLoopbackTransport() override;

	bool             Listen(uint16 Port) override;
	FNetConnectionId Connect(const std::string& Address) override;
	bool             Send(FNetConnectionId Connection, const void* Data, uint32 Size, ENetReliability Reliability) override;
	void             Disconnect(FNetConnectionId Connection, const std::string& Reason) override;
	void             Poll(std::vector<FNetEvent>& OutEvents) override;
	void             Close() override;

private:
	struct FPeer
	{
		FLoopbackTransport* Transport = nullptr;
		FNetConnectionId    RemoteId  = InvalidNetConnection; // 상대 쪽에서 본 같은 연결의 ID
	};

	void Deliver(FNetEvent&& Event) { Pending.push_back(std::move(Event)); }

	std::shared_ptr<FLoopbackHub>                  Hub;
	uint16                                         ListenPort = 0;
	std::unordered_map<FNetConnectionId, FPeer>    Peers;
	std::vector<FNetEvent>                         Pending;
};

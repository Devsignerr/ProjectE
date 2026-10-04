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

// 메모리 전송 계층 (테스트/프로세스 내부용). 주소는 "아무거나:포트" 형식이며 포트만 본다.
// 기본: 메시지는 손실·재정렬 없이 상대의 다음 Poll에 도착한다.
// 시뮬레이션 (SetSimulation — 보내는 쪽에 건다, GNS의 FakePacketLag/Loss_Send 흉내. 시드 고정이라 결정적):
//   시간 = 받는 쪽 Poll 횟수 × PollIntervalMs (Poll 한 번 = 한 프레임으로 본다).
//   비신뢰: LossPercent 확률로 사라지고, 남은 것은 지연 + 흔들림(0 ~ 지연 × JitterScale) 뒤 도착 — 서로 순서가 바뀔 수 있다.
//   신뢰: 사라지지 않는다. 대신 "잃을" 때마다 재전송 지연(왕복 + 한 프레임)이 더해지고, 같은 연결의 신뢰 메시지는 보낸 순서대로만 나온다
//     (앞 것이 도착할 때까지 뒤 것을 붙잡는다). 비신뢰는 신뢰를 앞지를 수 있다 — 신뢰 Welcome이 재전송되는 사이 비신뢰 스냅샷이 먼저 온다.
//   연결/끊김 이벤트는 바로 전달한다.
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
	void             SetSimulation(int32 LatencyMs, float LossPercent) override;

	// 시뮬레이션 난수 시드 / 받는 쪽 Poll 한 번의 시간 / 비신뢰 흔들림 배율 (테스트)
	void SetSimulationSeed(uint64 Seed) { RandomState = Seed; }
	void SetPollIntervalMs(double Milliseconds) { PollIntervalMs = Milliseconds; }
	void SetJitterScale(float Scale) { JitterScale = Scale; }

private:
	struct FPeer
	{
		FLoopbackTransport* Transport = nullptr;
		FNetConnectionId    RemoteId  = InvalidNetConnection; // 상대 쪽에서 본 같은 연결의 ID
		uint64              NextReliableSend = 0;             // 보내는 쪽: 다음 신뢰 순번
	};
	// 받는 쪽에 날아오는 중인 메시지 (시뮬레이션 중에만)
	struct FInFlight
	{
		double    DeliverAtMs = 0.0;
		uint64    Order       = 0;     // 보낸 순서 (같은 시각이면 먼저 보낸 것)
		bool      bReliable   = false;
		uint64    ReliableSeq = 0;     // 연결별 신뢰 순번
		FNetEvent Event;
	};

	void   Deliver(FNetEvent&& Event) { Pending.push_back(std::move(Event)); }
	void   Receive(FInFlight&& Message) { InFlight.push_back(std::move(Message)); }
	double NextRandom(); // [0, 1)

	std::shared_ptr<FLoopbackHub>               Hub;
	uint16                                      ListenPort = 0;
	std::unordered_map<FNetConnectionId, FPeer> Peers;
	std::vector<FNetEvent>                      Pending;

	// 시뮬레이션 (보내는 쪽 설정 + 받는 쪽 시계·대기열)
	int32                                        SimLatencyMs  = 0;
	float                                        SimLossPercent = 0.0f;
	float                                        JitterScale    = 0.25f;
	uint64                                       RandomState    = 0x9E3779B97F4A7C15ull;
	double                                       PollIntervalMs = 1000.0 / 60.0;
	double                                       NowMs          = 0.0;   // 받는 쪽 시계
	uint64                                       NextOrder      = 0;
	std::vector<FInFlight>                       InFlight;
	std::unordered_map<FNetConnectionId, uint64> NextReliableReceive; // 받는 쪽: 연결별 다음에 내보낼 신뢰 순번
};

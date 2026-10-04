#include "Network/LoopbackTransport.h"

#include <algorithm>
#include <charconv>

FLoopbackTransport::FLoopbackTransport(std::shared_ptr<FLoopbackHub> InHub)
	: Hub(std::move(InHub))
{
}

FLoopbackTransport::~FLoopbackTransport()
{
	Close();
}

bool FLoopbackTransport::Listen(uint16 Port)
{
	if (Port == 0 || ListenPort != 0 || Hub->Listeners.contains(Port))
	{
		return false;
	}
	ListenPort           = Port;
	Hub->Listeners[Port] = this;
	return true;
}

FNetConnectionId FLoopbackTransport::Connect(const std::string& Address)
{
	const size_t Colon = Address.rfind(':');
	uint16       Port  = 0;
	if (Colon == std::string::npos ||
	    std::from_chars(Address.data() + Colon + 1, Address.data() + Address.size(), Port).ec != std::errc() || Port == 0)
	{
		return InvalidNetConnection;
	}

	const FNetConnectionId LocalId = Hub->NextConnection++;
	const auto             Found   = Hub->Listeners.find(Port);
	if (Found == Hub->Listeners.end())
	{
		// 실제 네트워크처럼 접속 시도는 시작되고 실패는 이벤트로 알린다
		Deliver({ ENetEventType::Disconnected, LocalId, {}, "대기 중인 서버 없음" });
		return LocalId;
	}

	FLoopbackTransport*    Server   = Found->second;
	const FNetConnectionId RemoteId = Hub->NextConnection++;
	Peers[LocalId]                  = { Server, RemoteId };
	Server->Peers[RemoteId]         = { this, LocalId };
	Server->Deliver({ ENetEventType::Connected, RemoteId, {}, {} });
	Deliver({ ENetEventType::Connected, LocalId, {}, {} });
	return LocalId;
}

double FLoopbackTransport::NextRandom()
{
	// SplitMix64 (표준 분포 함수 없이 결정적)
	uint64 Z = (RandomState += 0x9E3779B97F4A7C15ull);
	Z        = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
	Z        = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
	Z        = Z ^ (Z >> 31);
	return static_cast<double>(Z >> 11) * (1.0 / 9007199254740992.0);
}

bool FLoopbackTransport::Send(FNetConnectionId Connection, const void* Data, uint32 Size, ENetReliability Reliability)
{
	const auto Found = Peers.find(Connection);
	if (Found == Peers.end())
	{
		return false;
	}
	const uint8* Bytes = static_cast<const uint8*>(Data);
	FNetEvent    Event{ ENetEventType::Message, Found->second.RemoteId, std::vector<uint8>(Bytes, Bytes + Size), {} };
	if (SimLatencyMs <= 0 && SimLossPercent <= 0.0f)
	{
		Found->second.Transport->Deliver(std::move(Event));
		return true;
	}
	const bool   bReliable = Reliability == ENetReliability::Reliable;
	const double Latency   = static_cast<double>(std::max(SimLatencyMs, 0));
	double       Delay     = Latency + NextRandom() * Latency * JitterScale;
	if (bReliable)
	{
		// 신뢰: 잃을 때마다 재전송 (왕복 + 한 프레임 뒤)
		while (NextRandom() * 100.0 < SimLossPercent)
		{
			Delay += 2.0 * Latency + PollIntervalMs;
		}
	}
	else if (NextRandom() * 100.0 < SimLossPercent)
	{
		return true; // 비신뢰 손실 (보낸 쪽은 성공으로 안다)
	}
	FLoopbackTransport* Receiver = Found->second.Transport;
	FInFlight           Message;
	Message.DeliverAtMs = Receiver->NowMs + Delay;
	Message.Order       = Receiver->NextOrder++;
	Message.bReliable   = bReliable;
	Message.ReliableSeq = bReliable ? Found->second.NextReliableSend++ : 0;
	Message.Event       = std::move(Event);
	Receiver->Receive(std::move(Message));
	return true;
}

void FLoopbackTransport::Disconnect(FNetConnectionId Connection, const std::string& Reason)
{
	const auto Found = Peers.find(Connection);
	if (Found == Peers.end())
	{
		return;
	}
	const FPeer Peer = Found->second;
	Peers.erase(Found);
	Peer.Transport->Peers.erase(Peer.RemoteId);
	FNetEvent Event{ ENetEventType::Disconnected, Peer.RemoteId, {}, Reason };
	if (SimLatencyMs <= 0 && SimLossPercent <= 0.0f)
	{
		Peer.Transport->Deliver(std::move(Event));
		return;
	}
	// 시뮬레이션 중: 끊김도 앞서 보낸 신뢰 메시지(거부 사유 등) 뒤에 도착한다
	FInFlight Message;
	Message.DeliverAtMs = Peer.Transport->NowMs + static_cast<double>(std::max(SimLatencyMs, 0));
	Message.Order       = Peer.Transport->NextOrder++;
	Message.bReliable   = true;
	Message.ReliableSeq = Peer.NextReliableSend;
	Message.Event       = std::move(Event);
	Peer.Transport->Receive(std::move(Message));
}

void FLoopbackTransport::Poll(std::vector<FNetEvent>& OutEvents)
{
	for (FNetEvent& Event : Pending)
	{
		OutEvents.push_back(std::move(Event));
	}
	Pending.clear();
	NowMs += PollIntervalMs;
	if (InFlight.empty())
	{
		return;
	}
	std::stable_sort(InFlight.begin(), InFlight.end(), [](const FInFlight& A, const FInFlight& B) {
		return A.DeliverAtMs != B.DeliverAtMs ? A.DeliverAtMs < B.DeliverAtMs : A.Order < B.Order;
	});
	// 도착한 것을 시각 순으로. 신뢰는 연결별 순번 차례가 올 때까지 붙잡는다 (앞 것이 나오면 다시 훑는다)
	std::vector<bool> Delivered(InFlight.size(), false);
	for (bool bProgress = true; bProgress;)
	{
		bProgress = false;
		for (size_t Index = 0; Index < InFlight.size(); ++Index)
		{
			FInFlight& Message = InFlight[Index];
			if (Delivered[Index] || Message.DeliverAtMs > NowMs)
			{
				continue;
			}
			if (Message.bReliable)
			{
				uint64& Expected = NextReliableReceive[Message.Event.Connection];
				if (Message.ReliableSeq != Expected)
				{
					continue;
				}
				++Expected;
				bProgress = true;
			}
			OutEvents.push_back(std::move(Message.Event));
			Delivered[Index] = true;
		}
	}
	size_t Kept = 0;
	for (size_t Index = 0; Index < InFlight.size(); ++Index)
	{
		if (!Delivered[Index])
		{
			if (Kept != Index)
			{
				InFlight[Kept] = std::move(InFlight[Index]);
			}
			++Kept;
		}
	}
	InFlight.resize(Kept);
}

void FLoopbackTransport::Close()
{
	while (!Peers.empty())
	{
		Disconnect(Peers.begin()->first, "종료");
	}
	if (ListenPort != 0)
	{
		Hub->Listeners.erase(ListenPort);
		ListenPort = 0;
	}
	Pending.clear();
	InFlight.clear();
	NextReliableReceive.clear();
}

void FLoopbackTransport::SetSimulation(int32 LatencyMs, float LossPercent)
{
	SimLatencyMs   = std::max(LatencyMs, 0);
	SimLossPercent = std::clamp(LossPercent, 0.0f, 100.0f);
}

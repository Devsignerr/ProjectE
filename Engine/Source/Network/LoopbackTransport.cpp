#include "Network/LoopbackTransport.h"

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

bool FLoopbackTransport::Send(FNetConnectionId Connection, const void* Data, uint32 Size, ENetReliability /*Reliability*/)
{
	const auto Found = Peers.find(Connection);
	if (Found == Peers.end())
	{
		return false;
	}
	const uint8* Bytes = static_cast<const uint8*>(Data);
	Found->second.Transport->Deliver({ ENetEventType::Message, Found->second.RemoteId, std::vector<uint8>(Bytes, Bytes + Size), {} });
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
	Peer.Transport->Deliver({ ENetEventType::Disconnected, Peer.RemoteId, {}, Reason });
}

void FLoopbackTransport::Poll(std::vector<FNetEvent>& OutEvents)
{
	for (FNetEvent& Event : Pending)
	{
		OutEvents.push_back(std::move(Event));
	}
	Pending.clear();
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
}

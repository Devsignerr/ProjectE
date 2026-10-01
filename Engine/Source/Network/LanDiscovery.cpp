#include "Network/LanDiscovery.h"

#include "Core/Platform/WindowsHeaders.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Network/NetDriver.h"
#include "Network/NetMessages.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <format>
#include <random>

namespace
{
	constexpr uint32 QueryMagic = 0x51454A50; // "PJEQ" (리틀 엔디언)
	constexpr uint32 ReplyMagic = 0x52454A50; // "PJER"

	// Winsock 초기화 참조 카운트 (GNS도 따로 초기화하지만 WSAStartup은 중첩 호출을 허용한다)
	int32 GWinsockRefCount = 0;

	bool AcquireWinsock()
	{
		if (GWinsockRefCount++ == 0)
		{
			WSADATA Data;
			if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
			{
				GWinsockRefCount = 0;
				E_LOG(LogNet, Error, "Winsock 초기화 실패");
				return false;
			}
		}
		return true;
	}

	void ReleaseWinsock()
	{
		if (GWinsockRefCount > 0 && --GWinsockRefCount == 0)
		{
			WSACleanup();
		}
	}

	SOCKET OpenUdpSocket(uint16 BindPort, bool bBroadcast)
	{
		SOCKET Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (Socket == INVALID_SOCKET)
		{
			return INVALID_SOCKET;
		}
		const BOOL Enable = TRUE;
		setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&Enable), sizeof(Enable)); // 같은 PC의 여러 호스트
		if (bBroadcast)
		{
			setsockopt(Socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&Enable), sizeof(Enable));
		}
		u_long NonBlocking = 1;
		ioctlsocket(Socket, FIONBIO, &NonBlocking);

		sockaddr_in Address{};
		Address.sin_family      = AF_INET;
		Address.sin_addr.s_addr = htonl(INADDR_ANY);
		Address.sin_port        = htons(BindPort);
		if (bind(Socket, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == SOCKET_ERROR)
		{
			closesocket(Socket);
			return INVALID_SOCKET;
		}
		return Socket;
	}

	void SendTo(SOCKET Socket, const std::vector<uint8>& Bytes, uint32 HostOrderIp, uint16 Port)
	{
		sockaddr_in Address{};
		Address.sin_family      = AF_INET;
		Address.sin_addr.s_addr = htonl(HostOrderIp);
		Address.sin_port        = htons(Port);
		sendto(Socket, reinterpret_cast<const char*>(Bytes.data()), static_cast<int>(Bytes.size()), 0, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address));
	}
} // namespace

struct FLanDiscovery::FImpl
{
	bool         bWinsock = false;
	SOCKET       Socket   = INVALID_SOCKET;
	bool         bHosting = false;
	FLanHostInfo HostInfo;
	uint16       Players = 0;
	uint64       HostId  = 0;
	std::string  SearchProject;
};

FLanDiscovery::FLanDiscovery()
	: Impl(std::make_unique<FImpl>())
{
	Impl->bWinsock = AcquireWinsock();
}

FLanDiscovery::~FLanDiscovery()
{
	Stop();
	if (Impl->bWinsock)
	{
		ReleaseWinsock();
	}
}

bool FLanDiscovery::StartHost(const FLanHostInfo& Info, uint16 DiscoveryPort)
{
	Stop();
	DiscoveryPort = DiscoveryPort != 0 ? DiscoveryPort : GetConfiguredLanDiscoveryPort();
	if (!Impl->bWinsock || (Impl->Socket = OpenUdpSocket(DiscoveryPort, false)) == INVALID_SOCKET)
	{
		E_LOG(LogNet, Warning, "LAN 탐색 포트 {}를 열지 못했습니다 (방 목록에 보이지 않지만 직접 접속은 된다)", DiscoveryPort);
		return false;
	}
	Impl->bHosting = true;
	Impl->HostInfo = Info;
	std::random_device Random;
	Impl->HostId = (static_cast<uint64>(Random()) << 32) | Random();
	E_LOG(LogNet, Display, "LAN에 방 알림: '{}' (탐색 포트 {})", Info.Name, DiscoveryPort);
	return true;
}

void FLanDiscovery::SetPlayerCount(uint16 Players)
{
	Impl->Players = Players;
}

bool FLanDiscovery::StartSearch(const std::string& ProjectName, uint16 DiscoveryPort)
{
	Stop();
	DiscoveryPort = DiscoveryPort != 0 ? DiscoveryPort : GetConfiguredLanDiscoveryPort();
	Sessions.clear();
	if (!Impl->bWinsock || (Impl->Socket = OpenUdpSocket(0, true)) == INVALID_SOCKET)
	{
		E_LOG(LogNet, Warning, "LAN 검색 소켓을 열지 못했습니다");
		return false;
	}
	Impl->SearchProject = ProjectName;

	FBinaryWriter Query;
	Query.Write(QueryMagic);
	Query.Write(NetProtocolVersion);
	Query.WriteString(ProjectName);
	SendTo(Impl->Socket, Query.GetBuffer(), INADDR_BROADCAST, DiscoveryPort);
	SendTo(Impl->Socket, Query.GetBuffer(), INADDR_LOOPBACK, DiscoveryPort); // 같은 PC (브로드캐스트가 막힌 환경 대비)
	return true;
}

void FLanDiscovery::Update()
{
	if (Impl->Socket == INVALID_SOCKET)
	{
		return;
	}
	uint8 Buffer[1500];
	for (;;)
	{
		sockaddr_in From{};
		int         FromLength = sizeof(From);
		const int   Received   = recvfrom(Impl->Socket, reinterpret_cast<char*>(Buffer), sizeof(Buffer), 0, reinterpret_cast<sockaddr*>(&From), &FromLength);
		if (Received <= 0)
		{
			break; // WSAEWOULDBLOCK = 더 없음
		}
		FBinaryReader Reader(Buffer, static_cast<size_t>(Received));
		const uint32  Magic   = Reader.Read<uint32>();
		const uint32  Version = Reader.Read<uint32>();
		if (!Reader.IsOk() || Version != NetProtocolVersion)
		{
			continue; // 다른 버전의 엔진은 목록에 보이지 않는다 (접속해도 거부된다)
		}

		if (Impl->bHosting && Magic == QueryMagic)
		{
			const std::string Project = Reader.ReadString();
			if (!Reader.IsOk() || Project != Impl->HostInfo.Session.ProjectName)
			{
				continue;
			}
			FBinaryWriter Reply;
			Reply.Write(ReplyMagic);
			Reply.Write(NetProtocolVersion);
			Reply.Write(Impl->HostId);
			Reply.WriteString(Impl->HostInfo.Name);
			Reply.WriteString(Impl->HostInfo.Session.ProjectName);
			Reply.WriteString(Impl->HostInfo.Session.EngineVersion);
			Reply.WriteString(Impl->HostInfo.Session.SceneAsset);
			Reply.Write(Impl->HostInfo.GamePort);
			Reply.Write(Impl->Players);
			Reply.Write(Impl->HostInfo.MaxPlayers);
			sendto(Impl->Socket, reinterpret_cast<const char*>(Reply.GetBuffer().data()), static_cast<int>(Reply.GetBuffer().size()), 0,
			       reinterpret_cast<const sockaddr*>(&From), FromLength);
		}
		else if (!Impl->bHosting && Magic == ReplyMagic)
		{
			FLanSession Session;
			Session.HostId           = Reader.Read<uint64>();
			Session.Name             = Reader.ReadString();
			Session.ProjectName      = Reader.ReadString();
			Session.EngineVersion    = Reader.ReadString();
			Session.SceneAsset       = Reader.ReadString();
			const uint16 GamePort    = Reader.Read<uint16>();
			Session.Players          = Reader.Read<uint16>();
			Session.MaxPlayers       = Reader.Read<uint16>();
			if (!Reader.IsOk() || Session.ProjectName != Impl->SearchProject)
			{
				continue;
			}
			char Ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &From.sin_addr, Ip, sizeof(Ip));
			Session.Address = std::format("{}:{}", Ip, GamePort);
			// 같은 호스트가 브로드캐스트와 루프백 양쪽으로 답할 수 있다 — 호스트 ID로 하나만, LAN 주소 우선
			const bool bLoopback = From.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
			const auto Known     = std::find_if(Sessions.begin(), Sessions.end(), [&](const FLanSession& Other) { return Other.HostId == Session.HostId; });
			if (Known == Sessions.end())
			{
				Sessions.push_back(std::move(Session));
			}
			else if (!bLoopback)
			{
				*Known = std::move(Session);
			}
		}
	}
}

void FLanDiscovery::Stop()
{
	if (Impl->Socket != INVALID_SOCKET)
	{
		closesocket(Impl->Socket);
		Impl->Socket = INVALID_SOCKET;
	}
	Impl->bHosting = false;
}

bool FLanDiscovery::IsHosting() const
{
	return Impl->bHosting;
}

bool FLanDiscovery::IsSearching() const
{
	return Impl->Socket != INVALID_SOCKET && !Impl->bHosting;
}

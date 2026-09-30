#include "Core/Log.h"
#include "Core/Testing/TestFramework.h"

#pragma warning(push, 0)
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

E_DECLARE_LOG_CATEGORY(LogNetworkTest)
E_DEFINE_LOG_CATEGORY(LogNetworkTest, Log)

namespace
{
	// 연결 상태 콜백은 함수 포인터라 테스트 상태를 파일 전역에 둔다 (테스트는 단일 스레드에서 RunCallbacks로만 불린다)
	struct FEchoState
	{
		ISteamNetworkingSockets* Sockets    = nullptr;
		HSteamNetPollGroup       PollGroup  = k_HSteamNetPollGroup_Invalid;
		HSteamNetConnection      ServerSide = k_HSteamNetConnection_Invalid;
		HSteamNetConnection      Client     = k_HSteamNetConnection_Invalid;
		bool                     bClientConnected = false;
		bool                     bFailed          = false;
	};
	FEchoState GEcho;

	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Info)
	{
		const ESteamNetworkingConnectionState State = Info->m_info.m_eState;
		if (Info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid && State == k_ESteamNetworkingConnectionState_Connecting)
		{
			// 서버 쪽: 들어온 연결 수락
			if (GEcho.Sockets->AcceptConnection(Info->m_hConn) != k_EResultOK)
			{
				GEcho.bFailed = true;
				return;
			}
			GEcho.Sockets->SetConnectionPollGroup(Info->m_hConn, GEcho.PollGroup);
			GEcho.ServerSide = Info->m_hConn;
		}
		else if (Info->m_hConn == GEcho.Client && State == k_ESteamNetworkingConnectionState_Connected)
		{
			GEcho.bClientConnected = true;
		}
		else if (State == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || State == k_ESteamNetworkingConnectionState_ClosedByPeer)
		{
			GEcho.bFailed = true;
		}
	}

	void OnDebugOutput(ESteamNetworkingSocketsDebugOutputType Type, const char* Message)
	{
		E_LOG(LogNetworkTest, Warning, "[GNS {}] {}", static_cast<int>(Type), Message);
	}
} // namespace

E_TEST(Gns_LocalhostEcho)
{
	SteamDatagramErrMsg ErrorMessage;
	E_EXPECT_TRUE(GameNetworkingSockets_Init(nullptr, ErrorMessage));
	GEcho         = {};
	GEcho.Sockets = SteamNetworkingSockets();
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&OnConnectionStatusChanged);
	SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, &OnDebugOutput);

	// GNS는 포트 0(자동 할당)을 받지 않으므로 범위 안에서 비어 있는 포트를 찾는다
	SteamNetworkingIPAddr ListenAddress;
	ListenAddress.Clear();
	HSteamListenSocket Listen = k_HSteamListenSocket_Invalid;
	for (uint16 Port = 27500; Port < 27520 && Listen == k_HSteamListenSocket_Invalid; ++Port)
	{
		ListenAddress.Clear();
		ListenAddress.SetIPv4(0x7F000001, Port);
		Listen = GEcho.Sockets->CreateListenSocketIP(ListenAddress, 0, nullptr);
	}
	E_EXPECT_TRUE(Listen != k_HSteamListenSocket_Invalid);
	const SteamNetworkingIPAddr BoundAddress = ListenAddress;

	GEcho.PollGroup = GEcho.Sockets->CreatePollGroup();
	GEcho.Client    = GEcho.Sockets->ConnectByIPAddress(BoundAddress, 0, nullptr);
	E_EXPECT_TRUE(GEcho.Client != k_HSteamNetConnection_Invalid);

	// 클라이언트 → 서버 (신뢰) → 서버가 그대로 되돌려 보냄 → 클라이언트 수신
	const std::string Payload = "안녕 ProjectE";
	std::string       Echoed;
	bool              bSent      = false;
	const auto        Deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (Echoed.empty() && !GEcho.bFailed && std::chrono::steady_clock::now() < Deadline)
	{
		GEcho.Sockets->RunCallbacks();
		if (GEcho.bClientConnected && !bSent)
		{
			bSent = GEcho.Sockets->SendMessageToConnection(GEcho.Client, Payload.data(), static_cast<uint32>(Payload.size()),
			                                               k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK;
		}

		ISteamNetworkingMessage* Messages[8];
		const int ServerCount = GEcho.Sockets->ReceiveMessagesOnPollGroup(GEcho.PollGroup, Messages, 8);
		for (int Index = 0; Index < ServerCount; ++Index)
		{
			GEcho.Sockets->SendMessageToConnection(Messages[Index]->m_conn, Messages[Index]->m_pData, Messages[Index]->m_cbSize,
			                                       k_nSteamNetworkingSend_Reliable, nullptr);
			Messages[Index]->Release();
		}

		const int ClientCount = GEcho.Sockets->ReceiveMessagesOnConnection(GEcho.Client, Messages, 8);
		for (int Index = 0; Index < ClientCount; ++Index)
		{
			Echoed.assign(static_cast<const char*>(Messages[Index]->m_pData), static_cast<size_t>(Messages[Index]->m_cbSize));
			Messages[Index]->Release();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	E_EXPECT_FALSE(GEcho.bFailed);
	E_EXPECT_TRUE(bSent);
	E_EXPECT_TRUE(Echoed == Payload);

	GEcho.Sockets->CloseConnection(GEcho.Client, 0, nullptr, false);
	if (GEcho.ServerSide != k_HSteamNetConnection_Invalid)
	{
		GEcho.Sockets->CloseConnection(GEcho.ServerSide, 0, nullptr, false);
	}
	GEcho.Sockets->CloseListenSocket(Listen);
	GEcho.Sockets->DestroyPollGroup(GEcho.PollGroup);
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
	SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
	GameNetworkingSockets_Kill();
}

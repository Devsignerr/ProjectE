#include "Core/Testing/TestFramework.h"
#include "Network/NetTransport.h"

#include <chrono>
#include <string>
#include <thread>

namespace
{
	// 실제 소켓 테스트는 다른 프로세스와 겹치지 않도록 범위에서 빈 포트를 찾는다 (GNS는 포트 0을 받지 않음)
	uint16 ListenOnFreePort(INetTransport& Transport)
	{
		for (uint16 Port = 27500; Port < 27540; ++Port)
		{
			if (Transport.Listen(Port))
			{
				return Port;
			}
		}
		return 0;
	}
} // namespace

// GameNetworkingSockets 전송 계층: localhost 연결 → 신뢰 메시지 에코 → 끊김 이벤트
E_TEST(Gns_LocalhostEcho)
{
	if (!FTestRegistry::AllowRealSockets())
	{
		FTestRegistry::ReportSkipped("Gns_LocalhostEcho", "실제 소켓 — E_TEST_SOCKETS=1(-SocketTests)일 때만");
		return;
	}
	std::unique_ptr<INetTransport> Server = CreateGnsTransport();
	std::unique_ptr<INetTransport> Client = CreateGnsTransport();
	const uint16                   Port   = ListenOnFreePort(*Server);
	E_EXPECT_TRUE(Port != 0);
	const FNetConnectionId ToServer = Client->Connect("127.0.0.1:" + std::to_string(Port));
	E_EXPECT_TRUE(ToServer != InvalidNetConnection);

	const std::string      Payload = "안녕 ProjectE";
	std::string            Echoed;
	bool                   bSent    = false;
	std::vector<FNetEvent> Events;
	const auto             Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (Echoed.empty() && std::chrono::steady_clock::now() < Deadline)
	{
		Events.clear();
		Server->Poll(Events);
		for (const FNetEvent& Event : Events)
		{
			if (Event.Type == ENetEventType::Message) // 서버: 받은 그대로 되돌려 보냄
			{
				Server->Send(Event.Connection, Event.Data.data(), static_cast<uint32>(Event.Data.size()), ENetReliability::Reliable);
			}
		}
		Events.clear();
		Client->Poll(Events);
		for (const FNetEvent& Event : Events)
		{
			if (Event.Type == ENetEventType::Connected && Event.Connection == ToServer)
			{
				bSent = Client->Send(ToServer, Payload.data(), static_cast<uint32>(Payload.size()), ENetReliability::Reliable);
			}
			else if (Event.Type == ENetEventType::Message)
			{
				Echoed.assign(Event.Data.begin(), Event.Data.end());
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	E_EXPECT_TRUE(bSent);
	E_EXPECT_TRUE(Echoed == Payload);

	// 클라이언트가 닫으면 서버에 끊김 이벤트
	Client->Disconnect(ToServer, "테스트 종료");
	bool       bServerSawDisconnect = false;
	const auto CloseDeadline        = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!bServerSawDisconnect && std::chrono::steady_clock::now() < CloseDeadline)
	{
		Events.clear();
		Server->Poll(Events);
		for (const FNetEvent& Event : Events)
		{
			bServerSawDisconnect = bServerSawDisconnect || Event.Type == ENetEventType::Disconnected;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	E_EXPECT_TRUE(bServerSawDisconnect);
}

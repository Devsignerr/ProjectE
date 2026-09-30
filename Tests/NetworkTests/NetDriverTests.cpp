#include "Core/CommandLine.h"
#include "Core/Testing/TestFramework.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetDriver.h"

#include <chrono>
#include <thread>

namespace
{
	constexpr uint16 TestPort = 7777;

	FNetSessionInfo MakeSession(const std::string& PlayerName = {})
	{
		FNetSessionInfo Info;
		Info.EngineVersion = "0.1.0";
		Info.ProjectName   = "Sample";
		Info.SceneAsset    = "Scenes/Main.escene";
		Info.PlayerName    = PlayerName;
		return Info;
	}

	// 루프백은 한 번 갱신마다 한 단계씩 진행하므로 몇 번 돌려 핸드셰이크를 끝낸다
	void Pump(std::initializer_list<FNetDriver*> Drivers, int32 Rounds = 4, float DeltaSeconds = 1.0f / 60.0f)
	{
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			for (FNetDriver* Driver : Drivers)
			{
				Driver->Update(DeltaSeconds);
			}
		}
	}
} // namespace

E_TEST(NetMessages_RoundTripAndRejectGarbage)
{
	FNetHello Hello;
	Hello.EngineVersion = "0.1.0";
	Hello.ProjectName   = "Sample";
	Hello.SceneAsset    = "Scenes/한글.escene";
	Hello.PlayerName    = "호스트";
	const std::vector<uint8>       Bytes   = NetMessages::Encode(Hello);
	const std::optional<FNetHello> Decoded = NetMessages::DecodeHello(Bytes);
	E_EXPECT_TRUE(Decoded.has_value());
	E_EXPECT_TRUE(Decoded->ProtocolVersion == NetProtocolVersion && Decoded->SceneAsset == Hello.SceneAsset && Decoded->PlayerName == Hello.PlayerName);
	E_EXPECT_TRUE(NetMessages::PeekType(Bytes) == ENetMessageType::Hello);

	// 종류 불일치, 잘림, 남는 바이트는 실패
	E_EXPECT_FALSE(NetMessages::DecodeWelcome(Bytes).has_value());
	std::vector<uint8> Truncated(Bytes.begin(), Bytes.end() - 3);
	E_EXPECT_FALSE(NetMessages::DecodeHello(Truncated).has_value());
	std::vector<uint8> Padded = NetMessages::Encode(FNetWelcome{ 7 });
	E_EXPECT_EQ(NetMessages::DecodeWelcome(Padded)->PlayerId, 7u);
	Padded.push_back(0);
	E_EXPECT_FALSE(NetMessages::DecodeWelcome(Padded).has_value());
	E_EXPECT_FALSE(NetMessages::PeekType({}).has_value());
}

E_TEST(NetLaunchOptions_FromCommandLine)
{
	const FNetLaunchOptions None = FNetLaunchOptions::FromCommandLine(FCommandLine::Parse(L"--scene Scenes/A.escene"));
	E_EXPECT_TRUE(None.Mode == ENetMode::Standalone && None.Port == DefaultNetPort);
	const FNetLaunchOptions Host = FNetLaunchOptions::FromCommandLine(FCommandLine::Parse(L"--host --port 9000"));
	E_EXPECT_TRUE(Host.Mode == ENetMode::ListenServer && Host.Port == 9000);
	const FNetLaunchOptions Client = FNetLaunchOptions::FromCommandLine(FCommandLine::Parse(L"--connect 192.168.0.5:9000"));
	E_EXPECT_TRUE(Client.Mode == ENetMode::Client && Client.ConnectAddress == "192.168.0.5:9000");
	const FNetLaunchOptions BadPort = FNetLaunchOptions::FromCommandLine(FCommandLine::Parse(L"--host --port 99999"));
	E_EXPECT_EQ(BadPort.Port, DefaultNetPort);
}

E_TEST(Loopback_ConnectSendDisconnect)
{
	auto               Hub = std::make_shared<FLoopbackHub>();
	FLoopbackTransport Server(Hub);
	FLoopbackTransport Client(Hub);
	E_EXPECT_TRUE(Server.Listen(TestPort));
	E_EXPECT_FALSE(FLoopbackTransport(Hub).Listen(TestPort)); // 같은 포트 중복 대기 불가

	const FNetConnectionId ToServer = Client.Connect("127.0.0.1:7777");
	std::vector<FNetEvent> ServerEvents;
	std::vector<FNetEvent> ClientEvents;
	Server.Poll(ServerEvents);
	Client.Poll(ClientEvents);
	E_EXPECT_TRUE(ServerEvents.size() == 1 && ServerEvents[0].Type == ENetEventType::Connected);
	E_EXPECT_TRUE(ClientEvents.size() == 1 && ClientEvents[0].Type == ENetEventType::Connected && ClientEvents[0].Connection == ToServer);
	const FNetConnectionId ToClient = ServerEvents[0].Connection;

	const uint8 Bytes[3] = { 1, 2, 3 };
	E_EXPECT_TRUE(Client.Send(ToServer, Bytes, 3, ENetReliability::Unreliable));
	ServerEvents.clear();
	Server.Poll(ServerEvents);
	E_EXPECT_TRUE(ServerEvents.size() == 1 && ServerEvents[0].Connection == ToClient && ServerEvents[0].Data.size() == 3 && ServerEvents[0].Data[2] == 3);

	Server.Disconnect(ToClient, "추방");
	ClientEvents.clear();
	Client.Poll(ClientEvents);
	E_EXPECT_TRUE(ClientEvents.size() == 1 && ClientEvents[0].Type == ENetEventType::Disconnected && ClientEvents[0].Reason == "추방");
	E_EXPECT_FALSE(Client.Send(ToServer, Bytes, 3, ENetReliability::Reliable));

	// 대기 중인 서버가 없으면 접속 실패 이벤트
	const FNetConnectionId Nowhere = Client.Connect("127.0.0.1:1234");
	ClientEvents.clear();
	Client.Poll(ClientEvents);
	E_EXPECT_TRUE(ClientEvents.size() == 1 && ClientEvents[0].Type == ENetEventType::Disconnected && ClientEvents[0].Connection == Nowhere);
}

E_TEST(NetDriver_PlayersJoinAndLeave)
{
	auto       Hub = std::make_shared<FLoopbackHub>();
	FNetDriver Server;
	FNetDriver ClientA;
	FNetDriver ClientB;
	std::vector<uint32> Joined;
	std::vector<uint32> Left;
	Server.OnPlayerJoined = [&](const FNetDriver::FRemotePlayer& Player) { Joined.push_back(Player.PlayerId); };
	Server.OnPlayerLeft   = [&](const FNetDriver::FRemotePlayer& Player, const std::string&) { Left.push_back(Player.PlayerId); };

	E_EXPECT_TRUE(Server.GetMode() == ENetMode::Standalone && Server.IsServer()); // 시작 전 = Standalone (로컬이 서버)
	E_EXPECT_TRUE(Server.StartServer(std::make_unique<FLoopbackTransport>(Hub), TestPort, MakeSession(), false));
	E_EXPECT_TRUE(Server.GetMode() == ENetMode::ListenServer);
	E_EXPECT_EQ(Server.GetLocalPlayerId(), FNetDriver::HostPlayerId);
	E_EXPECT_TRUE(ClientA.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", MakeSession("앨리스")));
	E_EXPECT_TRUE(ClientB.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", MakeSession()));
	E_EXPECT_TRUE(ClientA.GetClientState() == FNetDriver::EClientState::Connecting && !ClientA.IsServer());
	Pump({ &Server, &ClientA, &ClientB });

	E_EXPECT_TRUE(ClientA.GetClientState() == FNetDriver::EClientState::Joined);
	E_EXPECT_TRUE(ClientB.GetClientState() == FNetDriver::EClientState::Joined);
	E_EXPECT_TRUE(ClientA.GetLocalPlayerId() != ClientB.GetLocalPlayerId());
	E_EXPECT_TRUE(ClientA.GetLocalPlayerId() != FNetDriver::HostPlayerId && ClientB.GetLocalPlayerId() != FNetDriver::HostPlayerId);
	E_EXPECT_EQ(Server.GetPlayers().size(), 2u);
	E_EXPECT_EQ(Joined.size(), 2u);
	E_EXPECT_TRUE(Server.GetPlayers()[0].Name == "앨리스" && Server.GetPlayers()[1].Name == "Player" + std::to_string(ClientB.GetLocalPlayerId()));

	// 클라이언트 종료 → 서버 퇴장 처리
	const uint32 LeavingId = ClientA.GetLocalPlayerId();
	ClientA.Shutdown();
	Pump({ &Server, &ClientB });
	E_EXPECT_TRUE(Left.size() == 1 && Left[0] == LeavingId);
	E_EXPECT_EQ(Server.GetPlayers().size(), 1u);

	// 서버 종료 → 남은 클라이언트는 실패(끊김) 상태
	Server.Shutdown();
	Pump({ &ClientB });
	E_EXPECT_TRUE(ClientB.GetClientState() == FNetDriver::EClientState::Failed);
	E_EXPECT_TRUE(ClientB.GetFailureReason().find("끊김") != std::string::npos);
}

E_TEST(NetDriver_RejectsMismatchedSession)
{
	auto       Hub = std::make_shared<FLoopbackHub>();
	FNetDriver Server;
	FNetDriver Client;
	E_EXPECT_TRUE(Server.StartServer(std::make_unique<FLoopbackTransport>(Hub), TestPort, MakeSession(), true));
	E_EXPECT_TRUE(Server.GetMode() == ENetMode::DedicatedServer);

	FNetSessionInfo Other = MakeSession();
	Other.SceneAsset      = "Scenes/Other.escene";
	E_EXPECT_TRUE(Client.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", Other));
	Pump({ &Server, &Client });
	E_EXPECT_TRUE(Client.GetClientState() == FNetDriver::EClientState::Failed);
	E_EXPECT_TRUE(Client.GetFailureReason().find("씬 불일치") != std::string::npos);
	E_EXPECT_EQ(Server.GetPlayers().size(), 0u);
}

E_TEST(NetDriver_HandshakeTimeout)
{
	// Hello를 보내지 않는 연결은 시간 초과로 끊긴다
	auto               Hub = std::make_shared<FLoopbackHub>();
	FNetDriver         Server;
	FLoopbackTransport Silent(Hub);
	E_EXPECT_TRUE(Server.StartServer(std::make_unique<FLoopbackTransport>(Hub), TestPort, MakeSession(), true));
	Silent.Connect("127.0.0.1:7777");
	Server.Update(0.1f);
	Server.Update(FNetDriver::HandshakeTimeoutSeconds);

	std::vector<FNetEvent> Events;
	Silent.Poll(Events);
	bool bRejected     = false;
	bool bDisconnected = false;
	for (const FNetEvent& Event : Events)
	{
		bRejected     = bRejected || (Event.Type == ENetEventType::Message && NetMessages::DecodeReject(Event.Data).has_value());
		bDisconnected = bDisconnected || Event.Type == ENetEventType::Disconnected;
	}
	E_EXPECT_TRUE(bRejected && bDisconnected);
}

E_TEST(NetDriver_JoinOverGns)
{
	// 실제 소켓(GNS)으로 서버 + 클라이언트 입장, 거부 사유 전달까지
	FNetDriver Server;
	uint16     Port = 0;
	for (uint16 Candidate = 27540; Candidate < 27580 && Port == 0; ++Candidate)
	{
		if (Server.StartServer(CreateGnsTransport(), Candidate, MakeSession(), true))
		{
			Port = Candidate;
		}
	}
	E_EXPECT_TRUE(Port != 0);

	FNetDriver      Client;
	FNetDriver      Wrong;
	FNetSessionInfo WrongSession = MakeSession();
	WrongSession.ProjectName     = "Other";
	E_EXPECT_TRUE(Client.StartClient(CreateGnsTransport(), "127.0.0.1:" + std::to_string(Port), MakeSession("GNS")));
	E_EXPECT_TRUE(Wrong.StartClient(CreateGnsTransport(), "127.0.0.1:" + std::to_string(Port), WrongSession));

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while ((Client.GetClientState() == FNetDriver::EClientState::Connecting || Wrong.GetClientState() == FNetDriver::EClientState::Connecting) &&
	       std::chrono::steady_clock::now() < Deadline)
	{
		Pump({ &Server, &Client, &Wrong }, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	E_EXPECT_TRUE(Client.GetClientState() == FNetDriver::EClientState::Joined);
	E_EXPECT_EQ(Server.GetPlayers().size(), 1u);
	E_EXPECT_TRUE(Wrong.GetClientState() == FNetDriver::EClientState::Failed);
	E_EXPECT_TRUE(Wrong.GetFailureReason().find("프로젝트 불일치") != std::string::npos);
}

E_TEST(NetDriver_RejectsWhenFull)
{
	auto       Hub = std::make_shared<FLoopbackHub>();
	FNetDriver Server;
	Server.MaxPlayers = 1;
	FNetDriver First;
	FNetDriver Second;
	E_EXPECT_TRUE(Server.StartServer(std::make_unique<FLoopbackTransport>(Hub), TestPort, MakeSession(), true));
	E_EXPECT_TRUE(First.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", MakeSession()));
	Pump({ &Server, &First });
	E_EXPECT_TRUE(Second.StartClient(std::make_unique<FLoopbackTransport>(Hub), "127.0.0.1:7777", MakeSession()));
	Pump({ &Server, &First, &Second });
	E_EXPECT_TRUE(First.GetClientState() == FNetDriver::EClientState::Joined);
	E_EXPECT_TRUE(Second.GetClientState() == FNetDriver::EClientState::Failed);
	E_EXPECT_TRUE(Second.GetFailureReason().find("가득") != std::string::npos);
	E_EXPECT_TRUE(FNetLaunchOptions::FromCommandLine(FCommandLine::Parse(L"--join-lan")).bJoinLan);
}

#include "Network/NetDriver.h"

#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Network/LanDiscovery.h"

#include <algorithm>
#include <charconv>
#include <format>

FNetSessionInfo FNetSessionInfo::FromProject(const std::string& SceneAsset, const std::string& PlayerName)
{
	FNetSessionInfo Info;
	if (FPaths::HasProject())
	{
		Info.EngineVersion = FPaths::GetProjectDescriptor().EngineVersion;
		Info.ProjectName   = FPaths::GetProjectName();
	}
	Info.SceneAsset = SceneAsset;
	Info.PlayerName = PlayerName;
	return Info;
}

uint16 GetConfiguredNetPort()
{
	const uint32 Port = FProjectSettings::Get().Network.DefaultPort;
	return Port >= 1 && Port <= 65535 ? static_cast<uint16>(Port) : DefaultNetPort;
}

uint16 GetConfiguredLanDiscoveryPort()
{
	const uint32 Port = FProjectSettings::Get().Network.LanDiscoveryPort;
	return Port >= 1 && Port <= 65535 ? static_cast<uint16>(Port) : DefaultLanDiscoveryPort;
}

FNetLaunchOptions FNetLaunchOptions::FromCommandLine(const FCommandLine& CommandLine)
{
	FNetLaunchOptions Options;
	Options.Port = GetConfiguredNetPort(); // 프로젝트 설정 기본 포트 (--port가 우선)
	if (const std::string PortText = FStringConv::ToUtf8(CommandLine.GetValue(L"--port")); !PortText.empty())
	{
		uint16 Port = 0;
		if (std::from_chars(PortText.data(), PortText.data() + PortText.size(), Port).ec == std::errc() && Port != 0)
		{
			Options.Port = Port;
		}
		else
		{
			E_LOG(LogNet, Warning, "--port 값이 잘못되어 기본 포트 {}를 씁니다: {}", Options.Port, PortText);
		}
	}
	if (const std::wstring Lag = CommandLine.GetValue(L"--net-lag"); !Lag.empty())
	{
		Options.SimulatedLatencyMs = std::max(0, std::stoi(Lag));
	}
	if (const std::wstring Loss = CommandLine.GetValue(L"--net-loss"); !Loss.empty())
	{
		Options.SimulatedLossPercent = std::clamp(std::stof(Loss), 0.0f, 100.0f);
	}
	if (const std::wstring Address = CommandLine.GetValue(L"--connect"); !Address.empty())
	{
		Options.Mode           = ENetMode::Client;
		Options.ConnectAddress = FStringConv::ToUtf8(Address);
	}
	else if (CommandLine.HasFlag(L"--join-lan"))
	{
		Options.Mode     = ENetMode::Client;
		Options.bJoinLan = true;
	}
	else if (CommandLine.HasFlag(L"--host"))
	{
		Options.Mode = ENetMode::ListenServer;
	}
	return Options;
}

FNetDriver::~FNetDriver()
{
	Shutdown();
}

bool FNetDriver::StartServer(std::unique_ptr<INetTransport> InTransport, uint16 Port, const FNetSessionInfo& Info, bool bDedicated)
{
	Shutdown();
	if (MaxPlayers == 0)
	{
		MaxPlayers = static_cast<uint16>(std::clamp<uint32>(FProjectSettings::Get().Network.MaxPlayers, 1u, 0xFFFFu)); // 프로젝트 설정
	}
	if (InTransport == nullptr || !InTransport->Listen(Port))
	{
		return false;
	}
	Transport     = std::move(InTransport);
	Mode          = bDedicated ? ENetMode::DedicatedServer : ENetMode::ListenServer;
	Session       = Info;
	LocalPlayerId = HostPlayerId;
	E_LOG(LogNet, Display, "{} 시작 (포트 {}, 씬 {})", ToString(Mode), Port, Info.SceneAsset);
	return true;
}

bool FNetDriver::StartClient(std::unique_ptr<INetTransport> InTransport, const std::string& Address, const FNetSessionInfo& Info)
{
	Shutdown();
	if (InTransport == nullptr)
	{
		return false;
	}
	const FNetConnectionId Connection = InTransport->Connect(Address);
	if (Connection == InvalidNetConnection)
	{
		return false;
	}
	Transport        = std::move(InTransport);
	Mode             = ENetMode::Client;
	Session          = Info;
	ServerConnection = Connection;
	ClientState      = EClientState::Connecting;
	E_LOG(LogNet, Display, "서버 {}에 접속 중...", Address);
	return true;
}

void FNetDriver::Shutdown()
{
	if (Transport != nullptr)
	{
		Transport->Close();
		Transport.reset();
	}
	Mode             = ENetMode::Standalone;
	PendingConnections.clear();
	Players.clear();
	NextPlayerId     = 1;
	ServerConnection = InvalidNetConnection;
	ClientState      = EClientState::Idle;
	LocalPlayerId    = HostPlayerId;
	FailureReason.clear();
	TravelCount           = 0;
	ClientTravelId        = 0;
	bClientTravelConsumed = false;
	ClientTravelScene.clear();
}

void FNetDriver::Update(float DeltaSeconds)
{
	if (Transport == nullptr)
	{
		return;
	}
	EventBuffer.clear();
	Transport->Poll(EventBuffer);
	for (const FNetEvent& Event : EventBuffer)
	{
		if (IsServer())
		{
			HandleServerEvent(Event);
		}
		else
		{
			HandleClientEvent(Event);
		}
	}

	if (IsServer())
	{
		// 연결만 하고 Hello를 보내지 않는 연결은 정리한다
		for (FPendingConnection& Pending : PendingConnections)
		{
			Pending.Elapsed += DeltaSeconds;
		}
		std::vector<FNetConnectionId> Expired;
		for (const FPendingConnection& Pending : PendingConnections)
		{
			if (Pending.Elapsed >= HandshakeTimeoutSeconds)
			{
				Expired.push_back(Pending.Connection);
			}
		}
		for (const FNetConnectionId Connection : Expired)
		{
			RejectConnection(Connection, "접속 요청 시간 초과");
		}
	}
}

void FNetDriver::HandleServerEvent(const FNetEvent& Event)
{
	const auto FindPending = [this](FNetConnectionId Connection) {
		return std::find_if(PendingConnections.begin(), PendingConnections.end(), [Connection](const FPendingConnection& Pending) { return Pending.Connection == Connection; });
	};
	const auto FindPlayer = [this](FNetConnectionId Connection) {
		return std::find_if(Players.begin(), Players.end(), [Connection](const FRemotePlayer& Player) { return Player.Connection == Connection; });
	};

	switch (Event.Type)
	{
	case ENetEventType::Connected:
		PendingConnections.push_back({ Event.Connection, 0.0f });
		break;

	case ENetEventType::Disconnected:
		if (const auto Pending = FindPending(Event.Connection); Pending != PendingConnections.end())
		{
			PendingConnections.erase(Pending);
		}
		else if (const auto Player = FindPlayer(Event.Connection); Player != Players.end())
		{
			const FRemotePlayer Left = *Player;
			Players.erase(Player);
			E_LOG(LogNet, Display, "플레이어 {} '{}' 퇴장: {}", Left.PlayerId, Left.Name, Event.Reason);
			if (OnPlayerLeft)
			{
				OnPlayerLeft(Left, Event.Reason);
			}
		}
		break;

	case ENetEventType::Message:
	{
		const auto Pending = FindPending(Event.Connection);
		if (Pending == PendingConnections.end())
		{
			const std::optional<ENetMessageType> Type   = NetMessages::PeekType(Event.Data);
			const auto                           Player = FindPlayer(Event.Connection);
			if (Player == Players.end() || !Type)
			{
				break;
			}
			if (*Type == ENetMessageType::TravelAck)
			{
				const std::optional<FNetTravelAck> Ack = NetMessages::DecodeTravelAck(Event.Data);
				if (Ack && Ack->TravelId == TravelCount && !Player->bInScene)
				{
					Player->bInScene            = true;
					const FRemotePlayer Arrived = *Player; // 콜백이 목록을 바꿀 수 있으므로 복사
					E_LOG(LogNet, Display, "플레이어 {} '{}' 맵 이동 완료: {}", Arrived.PlayerId, Arrived.Name, Session.SceneAsset);
					if (OnPlayerJoined)
					{
						OnPlayerJoined(Arrived); // 새 씬 입장 (폰 생성 + 전체 상태)
					}
				}
				break;
			}
			// 맵 이동 중인 플레이어의 메시지는 이전 씬 기준이므로 버린다
			if (Player->bInScene && *Type >= ENetMessageType::GameBase && OnGameMessage)
			{
				OnGameMessage(Event.Connection, Event.Data);
			}
			break;
		}
		const std::optional<FNetHello> Hello = NetMessages::DecodeHello(Event.Data);
		if (!Hello)
		{
			RejectConnection(Event.Connection, "프로토콜 오류: 접속 요청이 아님");
			break;
		}
		if (const std::string Problem = ValidateHello(*Hello); !Problem.empty())
		{
			RejectConnection(Event.Connection, Problem);
			break;
		}
		PendingConnections.erase(Pending);
		FRemotePlayer Player;
		Player.Connection = Event.Connection;
		Player.PlayerId   = NextPlayerId++;
		Player.Name       = Hello->PlayerName.empty() ? std::format("Player{}", Player.PlayerId) : Hello->PlayerName;
		Players.push_back(Player);

		const std::vector<uint8> Welcome = NetMessages::Encode(FNetWelcome{ Player.PlayerId });
		Transport->Send(Event.Connection, Welcome.data(), static_cast<uint32>(Welcome.size()), ENetReliability::Reliable);
		E_LOG(LogNet, Display, "플레이어 {} '{}' 입장 (현재 {}명)", Player.PlayerId, Player.Name, Players.size());
		if (OnPlayerJoined)
		{
			OnPlayerJoined(Player);
		}
		break;
	}
	}
}

void FNetDriver::HandleClientEvent(const FNetEvent& Event)
{
	if (Event.Connection != ServerConnection || ClientState == EClientState::Failed)
	{
		return;
	}
	switch (Event.Type)
	{
	case ENetEventType::Connected:
	{
		FNetHello Hello;
		Hello.EngineVersion            = Session.EngineVersion;
		Hello.ProjectName              = Session.ProjectName;
		Hello.SceneAsset               = Session.SceneAsset;
		Hello.PlayerName               = Session.PlayerName;
		const std::vector<uint8> Bytes = NetMessages::Encode(Hello);
		Transport->Send(ServerConnection, Bytes.data(), static_cast<uint32>(Bytes.size()), ENetReliability::Reliable);
		break;
	}
	case ENetEventType::Disconnected:
		FailClient(ClientState == EClientState::Joined ? "서버 연결 끊김: " + Event.Reason : "접속 실패: " + Event.Reason);
		break;
	case ENetEventType::Message:
		if (ClientState == EClientState::Joined)
		{
			const std::optional<ENetMessageType> Type = NetMessages::PeekType(Event.Data);
			if (Type && *Type == ENetMessageType::Travel)
			{
				if (const std::optional<FNetTravel> Travel = NetMessages::DecodeTravel(Event.Data))
				{
					ClientTravelId        = Travel->TravelId != 0 ? Travel->TravelId : 1;
					ClientTravelScene     = Travel->SceneAsset;
					bClientTravelConsumed = false; // 앱이 아직 열지 않았으면 마지막 지시만 남는다
					E_LOG(LogNet, Display, "서버가 맵을 바꿉니다: {}", ClientTravelScene);
				}
			}
			else if (Type && *Type >= ENetMessageType::GameBase && ClientTravelId == 0 && OnGameMessage) // 이동 중에는 버린다 (이전 씬 기준)
			{
				OnGameMessage(Event.Connection, Event.Data);
			}
		}
		else if (ClientState == EClientState::Connecting)
		{
			if (const std::optional<FNetWelcome> Welcome = NetMessages::DecodeWelcome(Event.Data))
			{
				ClientState   = EClientState::Joined;
				LocalPlayerId = Welcome->PlayerId;
				E_LOG(LogNet, Display, "서버 입장 (플레이어 {})", LocalPlayerId);
			}
			else if (const std::optional<FNetReject> Reject = NetMessages::DecodeReject(Event.Data))
			{
				FailClient("서버가 거부: " + Reject->Reason);
			}
			else
			{
				FailClient("프로토콜 오류: 알 수 없는 응답");
			}
		}
		break;
	}
}

bool FNetDriver::Send(FNetConnectionId Connection, const std::vector<uint8>& Message, ENetReliability Reliability)
{
	if (Transport == nullptr)
	{
		return false;
	}
	if (Mode == ENetMode::Client)
	{
		return SendToServer(Message, Reliability);
	}
	const auto Player = std::find_if(Players.begin(), Players.end(), [Connection](const FRemotePlayer& Remote) { return Remote.Connection == Connection; });
	if (Player != Players.end() && !Player->bInScene)
	{
		return false; // 맵 이동 중 (새 씬을 열기 전)
	}
	return Transport->Send(Connection, Message.data(), static_cast<uint32>(Message.size()), Reliability);
}

void FNetDriver::Broadcast(const std::vector<uint8>& Message, ENetReliability Reliability)
{
	if (Transport == nullptr || !IsServer())
	{
		return;
	}
	for (const FRemotePlayer& Player : Players)
	{
		if (Player.bInScene)
		{
			Transport->Send(Player.Connection, Message.data(), static_cast<uint32>(Message.size()), Reliability);
		}
	}
}

void FNetDriver::BeginServerTravel(const std::string& SceneAsset)
{
	Session.SceneAsset = SceneAsset; // 이후 입장(Hello) 확인 기준
	if (Transport == nullptr || !IsServer())
	{
		return;
	}
	const std::vector<uint8> Message = NetMessages::Encode(FNetTravel{ ++TravelCount, SceneAsset });
	for (FRemotePlayer& Player : Players)
	{
		Transport->Send(Player.Connection, Message.data(), static_cast<uint32>(Message.size()), ENetReliability::Reliable);
		Player.bInScene = false;
	}
	E_LOG(LogNet, Display, "맵 이동 {} → {} (따라올 플레이어 {}명)", TravelCount, SceneAsset, Players.size());
}

std::optional<std::string> FNetDriver::ConsumeServerTravel()
{
	if (ClientTravelId == 0 || bClientTravelConsumed)
	{
		return std::nullopt;
	}
	bClientTravelConsumed = true;
	return ClientTravelScene;
}

void FNetDriver::CompleteClientTravel()
{
	if (ClientTravelId == 0 || Transport == nullptr || Mode != ENetMode::Client)
	{
		return;
	}
	if (!bClientTravelConsumed)
	{
		return; // 여는 동안 서버가 또 이동을 지시했다 → 앱이 그 씬을 마저 연 뒤에 확인한다
	}
	Session.SceneAsset             = ClientTravelScene; // 다시 접속할 때도 이 씬
	const std::vector<uint8> Bytes = NetMessages::Encode(FNetTravelAck{ ClientTravelId });
	Transport->Send(ServerConnection, Bytes.data(), static_cast<uint32>(Bytes.size()), ENetReliability::Reliable);
	ClientTravelId        = 0;
	bClientTravelConsumed = false;
}

bool FNetDriver::SendToServer(const std::vector<uint8>& Message, ENetReliability Reliability)
{
	if (Transport == nullptr || Mode != ENetMode::Client || ClientState != EClientState::Joined)
	{
		return false;
	}
	return Transport->Send(ServerConnection, Message.data(), static_cast<uint32>(Message.size()), Reliability);
}

bool FNetDriver::GetStats(FNetConnectionId Connection, FNetConnectionStats& OutStats) const
{
	return Transport != nullptr && Transport->GetStats(Connection, OutStats);
}

void FNetDriver::SetSimulation(int32 LatencyMs, float LossPercent)
{
	if (Transport != nullptr)
	{
		Transport->SetSimulation(LatencyMs, LossPercent);
	}
}

std::string FNetDriver::ValidateHello(const FNetHello& Hello) const
{
	if (Hello.ProtocolVersion != NetProtocolVersion)
	{
		return std::format("네트워크 프로토콜 버전 불일치 (서버 {}, 클라이언트 {})", NetProtocolVersion, Hello.ProtocolVersion);
	}
	if (Hello.EngineVersion != Session.EngineVersion)
	{
		return std::format("엔진 버전 불일치 (서버 {}, 클라이언트 {})", Session.EngineVersion, Hello.EngineVersion);
	}
	if (Hello.ProjectName != Session.ProjectName)
	{
		return std::format("프로젝트 불일치 (서버 {}, 클라이언트 {})", Session.ProjectName, Hello.ProjectName);
	}
	if (Hello.SceneAsset != Session.SceneAsset)
	{
		return std::format("씬 불일치 (서버 {}, 클라이언트 {})", Session.SceneAsset, Hello.SceneAsset);
	}
	if (Players.size() >= MaxPlayers)
	{
		return std::format("서버가 가득 참 ({}명)", MaxPlayers);
	}
	return {};
}

void FNetDriver::RejectConnection(FNetConnectionId Connection, const std::string& Reason)
{
	E_LOG(LogNet, Warning, "접속 거부 (연결 {}): {}", Connection, Reason);
	const std::vector<uint8> Bytes = NetMessages::Encode(FNetReject{ Reason });
	Transport->Send(Connection, Bytes.data(), static_cast<uint32>(Bytes.size()), ENetReliability::Reliable);
	Transport->Disconnect(Connection, Reason);
	std::erase_if(PendingConnections, [Connection](const FPendingConnection& Pending) { return Pending.Connection == Connection; });
}

void FNetDriver::FailClient(const std::string& Reason)
{
	ClientState   = EClientState::Failed;
	FailureReason = Reason;
	E_LOG(LogNet, Warning, "{}", Reason);
}

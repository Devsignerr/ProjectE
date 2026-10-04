#include "Network/NetTransport.h"

#include "Network/NetBindPolicy.h"

#pragma warning(push, 0)
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

E_DEFINE_LOG_CATEGORY(LogNet, Log)

const char* ToString(ENetMode Mode)
{
	switch (Mode)
	{
	case ENetMode::ListenServer:    return "ListenServer";
	case ENetMode::DedicatedServer: return "DedicatedServer";
	case ENetMode::Client:          return "Client";
	default:                        return "Standalone";
	}
}

namespace
{
	class FGnsTransport;

	// GNS 전역 상태: 라이브러리 초기화 참조 카운트 + 상태 콜백(함수 포인터)을 소유 전송 계층으로 보내는 표.
	// RunCallbacks는 프로세스의 모든 연결 콜백을 부르므로 어느 전송 계층이 불러도 소유자에게 전달된다
	struct FGnsGlobals
	{
		uint32                                                    RefCount = 0;
		std::unordered_map<HSteamListenSocket, FGnsTransport*>  ListenOwners;
		std::unordered_map<HSteamNetConnection, FGnsTransport*> ConnectionOwners;
	};
	FGnsGlobals GGns;

	// GNS의 Warning 단계는 디버그 빌드 성능 경고("lock held for ...ms")가 대부분이라 일반 로그로 낮추고, Important부터 경고로 올린다
	void OnGnsDebugOutput(ESteamNetworkingSocketsDebugOutputType Type, const char* Message)
	{
		const ELogVerbosity Verbosity = Type <= k_ESteamNetworkingSocketsDebugOutputType_Error       ? ELogVerbosity::Error
		                              : Type <= k_ESteamNetworkingSocketsDebugOutputType_Important ? ELogVerbosity::Warning
		                                                                                           : ELogVerbosity::Log;
		if (FLog::ShouldLog(LogNet, Verbosity))
		{
			FLog::Write(LogNet, Verbosity, std::string("[GNS] ") + Message);
		}
	}

	void OnGnsConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Info);

	bool AcquireGns()
	{
		if (GGns.RefCount == 0)
		{
			SteamDatagramErrMsg ErrorMessage;
			if (!GameNetworkingSockets_Init(nullptr, ErrorMessage))
			{
				E_LOG(LogNet, Error, "GameNetworkingSockets 초기화 실패: {}", ErrorMessage);
				return false;
			}
			SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, &OnGnsDebugOutput);
			SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&OnGnsConnectionStatusChanged);
		}
		++GGns.RefCount;
		return true;
	}

	void ReleaseGns()
	{
		if (GGns.RefCount > 0 && --GGns.RefCount == 0)
		{
			SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
			SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
			GameNetworkingSockets_Kill();
		}
	}

	class FGnsTransport final : public INetTransport
	{
	public:
		FGnsTransport()
		{
			bInitialized = AcquireGns();
			if (bInitialized)
			{
				Sockets   = SteamNetworkingSockets();
				PollGroup = Sockets->CreatePollGroup();
			}
		}

		~FGnsTransport() override
		{
			Close();
			if (bInitialized)
			{
				Sockets->DestroyPollGroup(PollGroup);
				ReleaseGns();
			}
		}

		bool Listen(uint16 Port) override
		{
			if (!bInitialized || ListenSocket != k_HSteamListenSocket_Invalid)
			{
				return false;
			}
			SteamNetworkingIPAddr Address;
			Address.Clear(); // 모든 주소 (IPv6 dual-stack)
			if (NetBindPolicy::IsLoopbackOnly())
			{
				Address.SetIPv4(0x7F000001, 0); // 127.0.0.1 — 같은 PC 전용 (방화벽 확인 창 없음)
			}
			Address.m_port = Port;
			ListenSocket   = Sockets->CreateListenSocketIP(Address, 0, nullptr);
			if (ListenSocket == k_HSteamListenSocket_Invalid)
			{
				E_LOG(LogNet, Error, "포트 {} 대기 실패 (사용 중이거나 0?)", Port);
				return false;
			}
			GGns.ListenOwners[ListenSocket] = this;
			E_LOG(LogNet, Display, "포트 {}에서 접속 대기", Port);
			return true;
		}

		FNetConnectionId Connect(const std::string& Address) override
		{
			if (!bInitialized)
			{
				return InvalidNetConnection;
			}
			SteamNetworkingIPAddr Remote;
			if (!Remote.ParseString(Address.c_str()) || Remote.m_port == 0)
			{
				E_LOG(LogNet, Error, "잘못된 접속 주소: '{}' (ip:port)", Address);
				return InvalidNetConnection;
			}
			const HSteamNetConnection Connection = Sockets->ConnectByIPAddress(Remote, 0, nullptr);
			if (Connection == k_HSteamNetConnection_Invalid)
			{
				return InvalidNetConnection;
			}
			Sockets->SetConnectionPollGroup(Connection, PollGroup);
			GGns.ConnectionOwners[Connection] = this;
			Connections.insert(Connection);
			return Connection;
		}

		bool Send(FNetConnectionId Connection, const void* Data, uint32 Size, ENetReliability Reliability) override
		{
			if (!bInitialized || !Connections.contains(Connection))
			{
				return false;
			}
			const int Flags = Reliability == ENetReliability::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
			return Sockets->SendMessageToConnection(Connection, Data, Size, Flags, nullptr) == k_EResultOK;
		}

		void Disconnect(FNetConnectionId Connection, const std::string& Reason) override
		{
			if (Connections.erase(Connection) > 0)
			{
				Sockets->CloseConnection(Connection, 0, Reason.c_str(), true); // true = 보낸 신뢰 메시지는 전달 후 닫기
				GGns.ConnectionOwners.erase(Connection);
			}
		}

		void Poll(std::vector<FNetEvent>& OutEvents) override
		{
			if (!bInitialized)
			{
				return;
			}
			Sockets->RunCallbacks(); // 상태 변화 → OnStatusChanged → PendingEvents
			for (FNetEvent& Event : PendingEvents)
			{
				OutEvents.push_back(std::move(Event));
			}
			PendingEvents.clear();

			ISteamNetworkingMessage* Messages[64];
			for (;;)
			{
				const int Count = Sockets->ReceiveMessagesOnPollGroup(PollGroup, Messages, 64);
				for (int Index = 0; Index < Count; ++Index)
				{
					FNetEvent& Event = OutEvents.emplace_back();
					Event.Type       = ENetEventType::Message;
					Event.Connection = Messages[Index]->m_conn;
					const uint8* Bytes = static_cast<const uint8*>(Messages[Index]->m_pData);
					Event.Data.assign(Bytes, Bytes + Messages[Index]->m_cbSize);
					Messages[Index]->Release();
				}
				if (Count < 64)
				{
					break;
				}
			}
		}

		void Close() override
		{
			if (!bInitialized)
			{
				return;
			}
			const bool bHadConnections = !Connections.empty();
			for (const HSteamNetConnection Connection : Connections)
			{
				Sockets->CloseConnection(Connection, 0, "종료", true);
				GGns.ConnectionOwners.erase(Connection);
			}
			Connections.clear();
			if (bHadConnections)
			{
				// 닫기 신호는 GNS 서비스 스레드가 보낸다 — 곧바로 GameNetworkingSockets_Kill(마지막 트랜스포트 해제)하면 상대가 연결 시간 초과까지
				// 끊긴 줄 모른다 (전용 서버에 클라이언트 플레이어가 남음). 종료·세션 전환 때 한 번이라 짧게 기다린다
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (ListenSocket != k_HSteamListenSocket_Invalid)
			{
				Sockets->CloseListenSocket(ListenSocket);
				GGns.ListenOwners.erase(ListenSocket);
				ListenSocket = k_HSteamListenSocket_Invalid;
			}
			PendingEvents.clear();
		}

		void OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t& Info)
		{
			const HSteamNetConnection Connection = Info.m_hConn;
			switch (Info.m_info.m_eState)
			{
			case k_ESteamNetworkingConnectionState_Connecting:
				// 서버: 대기 소켓으로 들어온 연결 수락 (Connected 이벤트는 수립 후)
				if (Info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid && !Connections.contains(Connection))
				{
					if (Sockets->AcceptConnection(Connection) != k_EResultOK)
					{
						Sockets->CloseConnection(Connection, 0, "수락 실패", false);
						return;
					}
					Sockets->SetConnectionPollGroup(Connection, PollGroup);
					GGns.ConnectionOwners[Connection] = this;
					Connections.insert(Connection);
				}
				break;
			case k_ESteamNetworkingConnectionState_Connected:
				PendingEvents.push_back({ ENetEventType::Connected, Connection, {}, {} });
				break;
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
				if (Connections.erase(Connection) > 0)
				{
					// 닫기 전에 도착해 있던 메시지(예: 거부 사유)를 끊김 이벤트보다 먼저 전달한다 (닫으면 버려진다)
					DrainMessages(Connection);
					PendingEvents.push_back({ ENetEventType::Disconnected, Connection, {}, Info.m_info.m_szEndDebug });
				}
				Sockets->CloseConnection(Connection, 0, nullptr, false);
				GGns.ConnectionOwners.erase(Connection);
				break;
			default:
				break;
			}
		}

		bool GetStats(FNetConnectionId Connection, FNetConnectionStats& OutStats) const override
		{
			SteamNetConnectionRealTimeStatus_t Status;
			if (!bInitialized || Sockets->GetConnectionRealTimeStatus(Connection, &Status, 0, nullptr) != k_EResultOK)
			{
				return false;
			}
			OutStats.PingMs         = Status.m_nPing;
			OutStats.Quality        = Status.m_flConnectionQualityLocal;
			OutStats.OutBytesPerSec = Status.m_flOutBytesPerSec;
			OutStats.InBytesPerSec  = Status.m_flInBytesPerSec;
			return true;
		}

		void SetSimulation(int32 LatencyMs, float LossPercent) override
		{
			if (!bInitialized)
			{
				return;
			}
			// 보내는 쪽에만 걸어 한 방향 지연 = LatencyMs (양쪽이 켜면 왕복은 두 배)
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, LatencyMs);
			SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, LossPercent);
			E_LOG(LogNet, Display, "네트워크 시뮬레이션: 지연 {}ms, 손실 {}%", LatencyMs, LossPercent);
		}

	private:
		void DrainMessages(HSteamNetConnection Connection)
		{
			ISteamNetworkingMessage* Messages[16];
			int                      Count = 0;
			while ((Count = Sockets->ReceiveMessagesOnConnection(Connection, Messages, 16)) > 0)
			{
				for (int Index = 0; Index < Count; ++Index)
				{
					const uint8* Bytes = static_cast<const uint8*>(Messages[Index]->m_pData);
					PendingEvents.push_back({ ENetEventType::Message, Connection, std::vector<uint8>(Bytes, Bytes + Messages[Index]->m_cbSize), {} });
					Messages[Index]->Release();
				}
			}
		}

		bool                                    bInitialized = false;
		ISteamNetworkingSockets*                Sockets      = nullptr;
		HSteamListenSocket                      ListenSocket = k_HSteamListenSocket_Invalid;
		HSteamNetPollGroup                      PollGroup    = k_HSteamNetPollGroup_Invalid;
		std::unordered_set<HSteamNetConnection> Connections;
		std::vector<FNetEvent>                  PendingEvents;
	};

	void OnGnsConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Info)
	{
		FGnsTransport* Owner = nullptr;
		if (const auto Found = GGns.ConnectionOwners.find(Info->m_hConn); Found != GGns.ConnectionOwners.end())
		{
			Owner = Found->second;
		}
		else if (const auto Listen = GGns.ListenOwners.find(Info->m_info.m_hListenSocket); Listen != GGns.ListenOwners.end())
		{
			Owner = Listen->second;
		}
		if (Owner != nullptr)
		{
			Owner->OnStatusChanged(*Info);
		}
	}
} // namespace

std::unique_ptr<INetTransport> CreateGnsTransport()
{
	return std::make_unique<FGnsTransport>();
}

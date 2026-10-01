#pragma once

#include "Network/NetMessages.h"
#include "Network/NetTransport.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class FCommandLine;

inline constexpr uint16 DefaultNetPort = 7777; // 프로젝트 설정이 없을 때 (실제 기본값은 GetConfiguredNetPort)

// 프로젝트 설정 "네트워크"의 값 (FProjectSettings::Network)
uint16 GetConfiguredNetPort();
uint16 GetConfiguredLanDiscoveryPort();

// 서버와 클라이언트가 같아야 하는 값 + 플레이어 이름 (Hello에 실려 서버가 확인한다)
struct FNetSessionInfo
{
	std::string EngineVersion;
	std::string ProjectName;
	std::string SceneAsset;
	std::string PlayerName; // 클라이언트만

	// 현재 프로젝트(.eproject의 Name/EngineVersion) + 씬
	static FNetSessionInfo FromProject(const std::string& SceneAsset, const std::string& PlayerName = {});
};

// 명령줄 실행 형태: --host [--port N] = 리슨 서버, --connect ip:port = 클라이언트, --join-lan = LAN에서 찾은 첫 세션에 접속,
// 없으면 Standalone. 전용 서버는 실행 파일이 정한다 (--port만 읽는다)
struct FNetLaunchOptions
{
	ENetMode    Mode = ENetMode::Standalone;
	uint16      Port = DefaultNetPort;
	std::string ConnectAddress;               // --join-lan이면 비어 있다 (앱이 찾아서 채운다)
	bool        bJoinLan             = false;
	int32       SimulatedLatencyMs   = 0;    // --net-lag <ms>: 디버그 지연 (보내는 쪽)
	float       SimulatedLossPercent = 0.0f; // --net-loss <%>: 디버그 손실

	static FNetLaunchOptions FromCommandLine(const FCommandLine& CommandLine);
};

// 연결 수립과 플레이어 목록을 관리한다 (메시지 복제/RPC는 이후 단계에서 위에 얹는다).
//   서버: Listen → 연결 → Hello 확인(프로토콜/엔진 버전, 프로젝트, 씬) → Welcome(플레이어 ID) 또는 Reject 후 연결 종료.
//         Hello가 HandshakeTimeoutSeconds 안에 오지 않으면 끊는다.
//   클라이언트: Connect → 연결되면 Hello → Welcome이면 Joined, Reject/끊김이면 Failed(사유)
// 시작하지 않으면 Standalone (네트워크 없음, 로컬이 서버)
class FNetDriver
{
public:
	enum class EClientState : uint8
	{
		Idle,
		Connecting, // 연결 중 또는 Welcome 대기
		Joined,
		Failed,
	};

	struct FRemotePlayer
	{
		FNetConnectionId Connection = InvalidNetConnection;
		uint32           PlayerId   = 0;
		std::string      Name;
		// 서버의 현재 씬에 들어와 있음. 맵 이동(BeginServerTravel) 중에는 false — TravelAck가 올 때까지
		// Broadcast/게임 메시지 수신에서 빠진다 (이전 씬의 NetId가 새 씬 엔티티에 섞이지 않게)
		bool bInScene = true;
	};

	static constexpr float  HandshakeTimeoutSeconds = 5.0f;
	uint16                  MaxPlayers              = 0; // 서버: 원격 플레이어 최대 수 (넘으면 "서버가 가득 참"으로 거부). 0 = 프로젝트 설정 "네트워크 → 최대 인원" (StartServer에서 정함)
	static constexpr uint32 HostPlayerId            = 0; // Standalone/리슨 서버의 로컬 플레이어. 원격 플레이어는 1부터

	FNetDriver() = default;
	~FNetDriver();

	FNetDriver(const FNetDriver&)            = delete;
	FNetDriver& operator=(const FNetDriver&) = delete;

	bool StartServer(std::unique_ptr<INetTransport> InTransport, uint16 Port, const FNetSessionInfo& Info, bool bDedicated);
	bool StartClient(std::unique_ptr<INetTransport> InTransport, const std::string& Address, const FNetSessionInfo& Info);
	void Shutdown();

	// 매 틱: 전송 계층 이벤트 처리, 핸드셰이크 시간 초과
	void Update(float DeltaSeconds);

	ENetMode GetMode() const { return Mode; }
	bool     IsServer() const { return Mode != ENetMode::Client; }

	// 서버: 입장한 원격 플레이어
	const std::vector<FRemotePlayer>& GetPlayers() const { return Players; }

	// 클라이언트 (Standalone/리슨 서버는 LocalPlayerId = HostPlayerId)
	EClientState       GetClientState() const { return ClientState; }
	uint32             GetLocalPlayerId() const { return LocalPlayerId; }
	const std::string& GetFailureReason() const { return FailureReason; }

	// 게임 메시지 보내기 (첫 바이트 = ENetMessageType::GameBase 이상). 서버: 입장한 플레이어에게, 클라이언트: 서버에게 (Connection 무시)
	bool Send(FNetConnectionId Connection, const std::vector<uint8>& Message, ENetReliability Reliability);
	void Broadcast(const std::vector<uint8>& Message, ENetReliability Reliability); // 서버: 입장한 모든 플레이어
	bool SendToServer(const std::vector<uint8>& Message, ENetReliability Reliability);

	// 디버그: 연결 상태 (클라이언트는 서버 연결), 지연/손실 흉내 (시작한 뒤에 부른다)
	bool GetStats(FNetConnectionId Connection, FNetConnectionStats& OutStats) const;
	bool GetServerStats(FNetConnectionStats& OutStats) const { return GetStats(ServerConnection, OutStats); }
	void SetSimulation(int32 LatencyMs, float LossPercent);

	// ---- 맵 이동 (언리얼 ServerTravel 방식, 연결 유지)
	//   서버: BeginServerTravel = 세션 씬을 바꾸고(이후 입장 확인 기준) 입장한 모두에게 Travel을 보낸 뒤 전원 bInScene = false.
	//         앱은 곧바로 새 씬을 연다. 클라이언트의 TravelAck(이번 이동 번호)가 오면 bInScene = true 후 OnPlayerJoined를 다시 부른다
	//         (앱의 입장 처리 = 폰 생성 + 전체 상태 전송이 그대로 새 씬 입장이 된다)
	//   클라이언트: Travel을 받으면 이후 게임 메시지를 버리고 ConsumeServerTravel이 새 씬을 돌려준다. 앱이 새 씬을 연 뒤
	//         CompleteClientTravel → TravelAck 전송, 세션 씬 갱신, 게임 메시지 수신 재개
	void                       BeginServerTravel(const std::string& SceneAsset);
	std::optional<std::string> ConsumeServerTravel();
	void                       CompleteClientTravel();
	bool                       IsClientTravelPending() const { return ClientTravelId != 0; }
	const std::string&         GetSessionScene() const { return Session.SceneAsset; }

	// 서버 이벤트 (Update 안에서 불린다)
	std::function<void(const FRemotePlayer&)>                     OnPlayerJoined;
	std::function<void(const FRemotePlayer&, const std::string&)> OnPlayerLeft; // 두 번째 인자: 사유
	// 게임 메시지 수신 (서버: 입장한 플레이어로부터, 클라이언트: 입장 후 서버로부터). Update 안에서 불린다
	std::function<void(FNetConnectionId, const std::vector<uint8>&)> OnGameMessage;

private:
	struct FPendingConnection
	{
		FNetConnectionId Connection = InvalidNetConnection;
		float            Elapsed    = 0.0f;
	};

	void        HandleServerEvent(const FNetEvent& Event);
	void        HandleClientEvent(const FNetEvent& Event);
	std::string ValidateHello(const FNetHello& Hello) const; // 빈 문자열 = 통과
	void        RejectConnection(FNetConnectionId Connection, const std::string& Reason);
	void        FailClient(const std::string& Reason);

	std::unique_ptr<INetTransport> Transport;
	ENetMode                       Mode = ENetMode::Standalone;
	FNetSessionInfo                Session;

	// 서버
	std::vector<FPendingConnection> PendingConnections;
	std::vector<FRemotePlayer>      Players;
	uint32                          NextPlayerId = 1;
	uint32                          TravelCount  = 0; // 서버: 마지막 맵 이동 번호 (TravelAck 대조)

	// 클라이언트
	FNetConnectionId ServerConnection = InvalidNetConnection;
	EClientState     ClientState      = EClientState::Idle;
	uint32           LocalPlayerId    = HostPlayerId;
	std::string      FailureReason;
	uint32           ClientTravelId = 0;          // 받은 이동 번호 (0 = 이동 중 아님). CompleteClientTravel까지 게임 메시지를 버린다
	std::string      ClientTravelScene;
	bool             bClientTravelConsumed = false; // ConsumeServerTravel로 앱에 넘겼음

	std::vector<FNetEvent> EventBuffer; // Poll 재사용 버퍼
};

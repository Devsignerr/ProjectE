#pragma once

#include "Network/NetMessages.h"
#include "Network/NetTransport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class FCommandLine;

inline constexpr uint16 DefaultNetPort = 7777;

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

// 명령줄 실행 형태: --host [--port N] = 리슨 서버, --connect ip:port = 클라이언트, 둘 다 없으면 Standalone.
// 전용 서버는 실행 파일이 정한다 (--port만 읽는다)
struct FNetLaunchOptions
{
	ENetMode    Mode = ENetMode::Standalone;
	uint16      Port = DefaultNetPort;
	std::string ConnectAddress;

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
	};

	static constexpr float  HandshakeTimeoutSeconds = 5.0f;
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

	// 서버 이벤트 (Update 안에서 불린다)
	std::function<void(const FRemotePlayer&)>                     OnPlayerJoined;
	std::function<void(const FRemotePlayer&, const std::string&)> OnPlayerLeft; // 두 번째 인자: 사유

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

	// 클라이언트
	FNetConnectionId ServerConnection = InvalidNetConnection;
	EClientState     ClientState      = EClientState::Idle;
	uint32           LocalPlayerId    = HostPlayerId;
	std::string      FailureReason;

	std::vector<FNetEvent> EventBuffer; // Poll 재사용 버퍼
};

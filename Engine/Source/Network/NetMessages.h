#pragma once

#include "Core/CoreTypes.h"

#include <optional>
#include <string>
#include <vector>

// 네트워크 메시지 형식: [uint8 종류][본문]. 본문은 FBinaryWriter 리틀 엔디언.
// 프로토콜을 바꾸면(메시지 추가/필드 변경) NetProtocolVersion을 올린다 — 버전이 다르면 접속을 거부한다
inline constexpr uint32 NetProtocolVersion = 6; // 2: 복제 메시지, 3: 트랜스폼 스냅샷, 4: RPC, 5: 입력 커맨드, 6: 입력에 시점 방향 + 카메라 Priority/강체 LockRotation

enum class ENetMessageType : uint8
{
	Hello   = 1, // 클라이언트 → 서버: 접속 요청 (버전/프로젝트/씬/이름)
	Welcome = 2, // 서버 → 클라이언트: 입장 허락 (플레이어 ID)
	Reject  = 3, // 서버 → 클라이언트: 거부 사유 (직후 연결을 닫는다)

	GameBase = 32, // 입장 후 게임 메시지는 여기부터 (FNetDriver::OnGameMessage로 전달)

	// 복제 (서버 → 클라이언트, 신뢰). 형식은 ReplicationServer.cpp 머리 주석
	ReplicationSpawn   = GameBase + 0,
	ReplicationDestroy = GameBase + 1,
	ReplicationState   = GameBase + 2,
	TransformSnapshot  = GameBase + 3, // 서버 → 클라이언트, 비신뢰 (최신 값만 의미 있음)
	ScriptRpc          = GameBase + 4, // 양방향, 신뢰. 형식은 World/GameWorld.cpp
	PlayerInput        = GameBase + 5, // 클라이언트 → 서버, 비신뢰 (매 틱 입력 상태 전체 + 시점 방향 yaw/pitch). 형식은 World/GameWorldNet.cpp
};

struct FNetHello
{
	uint32      ProtocolVersion = NetProtocolVersion;
	std::string EngineVersion;
	std::string ProjectName;
	std::string SceneAsset; // 클라이언트가 연 씬 (서버와 같아야 한다)
	std::string PlayerName;
};

struct FNetWelcome
{
	uint32 PlayerId = 0;
};

struct FNetReject
{
	std::string Reason;
};

namespace NetMessages
{
	std::vector<uint8> Encode(const FNetHello& Message);
	std::vector<uint8> Encode(const FNetWelcome& Message);
	std::vector<uint8> Encode(const FNetReject& Message);

	// 첫 바이트(종류). 빈 메시지면 nullopt
	std::optional<ENetMessageType> PeekType(const std::vector<uint8>& Data);

	// 종류가 다르거나 잘렸거나 뒤에 남는 바이트가 있으면 nullopt
	std::optional<FNetHello>   DecodeHello(const std::vector<uint8>& Data);
	std::optional<FNetWelcome> DecodeWelcome(const std::vector<uint8>& Data);
	std::optional<FNetReject>  DecodeReject(const std::vector<uint8>& Data);
}

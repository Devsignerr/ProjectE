#pragma once

#include "Core/CoreTypes.h"
#include "Core/Log.h"

#include <string>
#include <vector>

E_DECLARE_ENGINE_LOG_CATEGORY(LogNet)

// 실행 형태. Standalone(1인용)은 네트워크를 만들지 않고 로컬 프로세스가 서버이자 클라이언트로 취급된다 (UE NM_Standalone)
enum class ENetMode : uint8
{
	Standalone,
	ListenServer,    // 서버 + 로컬 플레이어 (호스트)
	DedicatedServer, // 서버만 (창/로컬 플레이어 없음)
	Client,
};

const char* ToString(ENetMode Mode);

enum class ENetReliability : uint8
{
	Reliable,   // 순서 보장 + 재전송
	Unreliable, // 손실 가능 (최신 상태만 의미 있는 데이터)
};

// 전송 계층 연결 식별자. 0은 무효
using FNetConnectionId                         = uint32;
inline constexpr FNetConnectionId InvalidNetConnection = 0;

enum class ENetEventType : uint8
{
	Connected,    // 연결 수립 (서버: 새 클라이언트, 클라이언트: 서버와 연결됨)
	Disconnected, // 끊김 또는 연결 실패 (Reason)
	Message,      // 메시지 수신 (Data)
};

struct FNetEvent
{
	ENetEventType      Type       = ENetEventType::Message;
	FNetConnectionId   Connection = InvalidNetConnection;
	std::vector<uint8> Data;
	std::string        Reason;
};

#pragma once

#include "Core/CoreTypes.h"
#include "Network/NetDriver.h"

#include <memory>
#include <string>
#include <vector>

inline constexpr uint16 DefaultLanDiscoveryPort = 7778;

// LAN에서 찾은 세션
struct FLanSession
{
	std::string Name;
	std::string ProjectName;
	std::string EngineVersion;
	std::string SceneAsset;
	std::string Address; // "ip:게임포트" — FNetDriver::StartClient에 그대로 넘긴다
	uint16      Players    = 0;
	uint16      MaxPlayers = 0;
	uint64      HostId     = 0; // 호스트 실행마다 무작위 (같은 호스트가 여러 경로로 답해도 한 번만)
};

// 호스트가 알리는 정보
struct FLanHostInfo
{
	std::string     Name;    // 방 이름 (목록 표시)
	FNetSessionInfo Session; // 프로젝트/엔진 버전/씬 (접속 조건과 같음)
	uint16          GamePort   = DefaultNetPort;
	uint16          MaxPlayers = 16;
};

// LAN 세션 찾기 (Winsock UDP, 소켓은 비차단 — 모든 호출은 메인 스레드에서).
//   호스트: 탐색 포트에서 질의를 기다렸다가 방 정보로 응답 (Update마다)
//   검색:   StartSearch가 브로드캐스트(255.255.255.255) + 127.0.0.1로 질의를 보내고, Update가 응답을 모은다.
//           같은 프로젝트의 세션만 받는다. 다시 부르면 목록을 비우고 다시 질의한다
// 질의:  uint32 'PJEQ', uint32 NetProtocolVersion, string 프로젝트
// 응답:  uint32 'PJER', uint32 NetProtocolVersion, uint64 호스트 ID, string 방 이름, string 프로젝트, string 엔진 버전, string 씬,
//        uint16 게임 포트, uint16 인원, uint16 최대 인원
// 같은 호스트가 브로드캐스트(LAN 주소)와 루프백 양쪽으로 답하면 호스트 ID로 하나만 남기고 LAN 주소를 쓴다
class FLanDiscovery
{
public:
	FLanDiscovery();
	~FLanDiscovery();

	FLanDiscovery(const FLanDiscovery&)            = delete;
	FLanDiscovery& operator=(const FLanDiscovery&) = delete;

	bool StartHost(const FLanHostInfo& Info, uint16 DiscoveryPort = DefaultLanDiscoveryPort);
	void SetPlayerCount(uint16 Players);

	bool StartSearch(const std::string& ProjectName, uint16 DiscoveryPort = DefaultLanDiscoveryPort);
	const std::vector<FLanSession>& GetSessions() const { return Sessions; }

	void Update();
	void Stop();
	bool IsHosting() const;
	bool IsSearching() const;

private:
	struct FImpl;
	std::unique_ptr<FImpl>   Impl;
	std::vector<FLanSession> Sessions;
};

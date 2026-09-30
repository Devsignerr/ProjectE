#pragma once

#include "Core/CoreTypes.h"

#include <memory>
#include <string>

// Steamworks 연동 (선택 기능).
//   - SDK는 파트너 계정으로만 받을 수 있어 저장소에 넣지 않는다. CMake 변수 E_STEAMWORKS_SDK_DIR(또는 루트 CMakeLocal.cmake,
//     환경 변수 STEAMWORKS_SDK_DIR)로 SDK를 지정해 빌드했을 때만 동작한다 (IsCompiledIn). 아니면 모든 함수가 조용히 "사용 불가".
//   - 프로세스 전역 하나 (Get). 런타임 앱이 .eproject "SteamAppId"로 Init(렌더러보다 먼저 — 오버레이가 D3D 장치를 잡도록)
//     → 매 프레임 RunCallbacks → 종료 때 Shutdown. 에디터는 초기화하지 않는다.
//   - Steam 헤더는 SteamSubsystem.cpp에서만 포함한다. 게임 모듈/Lua(`Steam` 테이블)는 이 클래스만 쓴다.
class FSteamSubsystem
{
public:
	enum class EInitResult : uint8
	{
		Disabled,            // AppId 0 또는 SDK 없이 빌드
		Ok,
		Failed,              // Steam 클라이언트가 꺼져 있음/로그인 안 됨/앱 소유 안 함 — 게임은 Steam 없이 계속
		RestartThroughSteam, // Steam 밖에서 실행된 패키지 게임: Steam이 다시 실행하므로 앱은 바로 종료해야 한다
	};

	static FSteamSubsystem& Get();
	static bool             IsCompiledIn();

	// bAllowRestart: 패키지 게임이 Steam 밖에서 실행되면 SteamAPI_RestartAppIfNecessary로 Steam을 통해 다시 실행.
	// 개발 실행은 false (AppId를 환경 변수 SteamAppId로 넘겨 steam_appid.txt 없이 초기화)
	EInitResult Init(uint32 AppId, bool bAllowRestart);
	void        Shutdown();
	void        RunCallbacks(); // 매 프레임

	bool        IsAvailable() const;
	uint32      GetAppId() const;
	std::string GetPlayerName() const; // 사용 불가면 빈 문자열
	uint64      GetSteamId() const;    // 사용 불가면 0
	std::string GetGameLanguage() const; // Steam 언어 이름 ("koreana", "english" …), 사용 불가면 빈 문자열

	// 업적: 이름은 Steamworks 파트너 사이트에 등록한 API 이름. 통계를 받기 전에 부르면 받은 뒤 적용한다 (대기열)
	bool UnlockAchievement(const std::string& Name);
	bool ClearAchievement(const std::string& Name); // 개발/테스트용
	bool IsAchievementUnlocked(const std::string& Name) const;

	// 오버레이: Dialog = "Friends", "Community", "Players", "Settings", "OfficialGameGroup", "Stats", "Achievements"
	bool ActivateOverlay(const std::string& Dialog);
	bool IsOverlayActive() const; // 오버레이가 떠 있으면 true (게임은 일시정지하는 것이 좋다)

	FSteamSubsystem(const FSteamSubsystem&)            = delete;
	FSteamSubsystem& operator=(const FSteamSubsystem&) = delete;

private:
	FSteamSubsystem();
	~FSteamSubsystem();

	struct FImpl;
	std::unique_ptr<FImpl> Impl;
};

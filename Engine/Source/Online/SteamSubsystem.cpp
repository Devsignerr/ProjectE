#include "Online/SteamSubsystem.h"

#include "Core/Log.h"

#if E_WITH_STEAM
	#include "Core/Platform/WindowsHeaders.h"

	#pragma warning(push, 0)
	#include <steam/steam_api.h>
	#pragma warning(pop)
#endif

#include <string>
#include <vector>

E_DEFINE_LOG_CATEGORY(LogOnline, Log)

#if E_WITH_STEAM

namespace
{
	// Steam 콜백은 SteamAPI_Init 이후에 등록해야 하므로 초기화 성공 후에 만든다
	class FSteamCallbacks
	{
	public:
		FSteamCallbacks(bool& bInStatsReady, bool& bInOverlayActive, std::vector<std::string>& InPending)
			: bStatsReady(bInStatsReady), bOverlayActive(bInOverlayActive), PendingAchievements(InPending)
		{
		}

	private:
		STEAM_CALLBACK(FSteamCallbacks, OnUserStatsReceived, UserStatsReceived_t);
		STEAM_CALLBACK(FSteamCallbacks, OnOverlayActivated, GameOverlayActivated_t);

		bool&                     bStatsReady;
		bool&                     bOverlayActive;
		std::vector<std::string>& PendingAchievements;
	};

	void FSteamCallbacks::OnUserStatsReceived(UserStatsReceived_t* Result)
	{
		if (Result->m_nGameID != SteamUtils()->GetAppID())
		{
			return;
		}
		if (Result->m_eResult != k_EResultOK)
		{
			E_LOG(LogOnline, Warning, "Steam 통계를 받지 못했습니다 (EResult {})", static_cast<int32>(Result->m_eResult));
			return;
		}
		bStatsReady = true;
		// 받기 전에 요청된 업적 적용
		for (const std::string& Name : PendingAchievements)
		{
			SteamUserStats()->SetAchievement(Name.c_str());
		}
		if (!PendingAchievements.empty())
		{
			SteamUserStats()->StoreStats();
			PendingAchievements.clear();
		}
	}

	void FSteamCallbacks::OnOverlayActivated(GameOverlayActivated_t* Result)
	{
		bOverlayActive = Result->m_bActive != 0;
		E_LOG(LogOnline, Log, "Steam 오버레이 {}", bOverlayActive ? "열림" : "닫힘");
	}
} // namespace

#endif

struct FSteamSubsystem::FImpl
{
	bool                     bInitialized   = false;
	bool                     bStatsReady    = false;
	bool                     bOverlayActive = false;
	uint32                   AppId          = 0;
	std::vector<std::string> PendingAchievements;
#if E_WITH_STEAM
	std::unique_ptr<FSteamCallbacks> Callbacks;
#endif
};

FSteamSubsystem::FSteamSubsystem()
	: Impl(std::make_unique<FImpl>())
{
}

FSteamSubsystem::~FSteamSubsystem() = default;

FSteamSubsystem& FSteamSubsystem::Get()
{
	static FSteamSubsystem Instance; // 엔진 DLL 안 하나 (게임 모듈도 같은 인스턴스)
	return Instance;
}

bool FSteamSubsystem::IsCompiledIn()
{
	return E_WITH_STEAM != 0;
}

FSteamSubsystem::EInitResult FSteamSubsystem::Init(uint32 AppId, bool bAllowRestart)
{
	if (Impl->bInitialized)
	{
		return EInitResult::Ok;
	}
	if (AppId == 0)
	{
		return EInitResult::Disabled;
	}
#if E_WITH_STEAM
	if (bAllowRestart && SteamAPI_RestartAppIfNecessary(AppId))
	{
		E_LOG(LogOnline, Display, "Steam 밖에서 실행되어 Steam을 통해 다시 실행합니다 (App ID {})", AppId);
		return EInitResult::RestartThroughSteam;
	}
	// steam_appid.txt 대신 환경 변수로 App ID를 넘긴다 (Steam이 실행한 경우에는 이미 설정되어 있다)
	const std::wstring AppIdText = std::to_wstring(AppId);
	SetEnvironmentVariableW(L"SteamAppId", AppIdText.c_str());
	SetEnvironmentVariableW(L"SteamGameId", AppIdText.c_str());
	if (!SteamAPI_Init())
	{
		E_LOG(LogOnline, Warning, "Steam 초기화 실패 (App ID {}): Steam 클라이언트 실행/로그인/앱 소유를 확인하세요. Steam 없이 계속합니다", AppId);
		return EInitResult::Failed;
	}
	Impl->bInitialized = true;
	Impl->AppId        = AppId;
	Impl->Callbacks    = std::make_unique<FSteamCallbacks>(Impl->bStatsReady, Impl->bOverlayActive, Impl->PendingAchievements);
	SteamUserStats()->RequestCurrentStats(); // 1.53: 업적을 쓰기 전에 통계를 받아야 한다 (UserStatsReceived_t)
	E_LOG(LogOnline, Display, "Steam 초기화 완료: App ID {}, 사용자 {}, 언어 {}", AppId, GetPlayerName(), GetGameLanguage());
	return EInitResult::Ok;
#else
	(void)bAllowRestart;
	E_LOG(LogOnline, Log, "Steamworks SDK 없이 빌드되어 Steam 기능을 쓰지 않습니다 (E_STEAMWORKS_SDK_DIR)");
	return EInitResult::Disabled;
#endif
}

void FSteamSubsystem::Shutdown()
{
	if (!Impl->bInitialized)
	{
		return;
	}
#if E_WITH_STEAM
	Impl->Callbacks.reset();
	SteamAPI_Shutdown();
#endif
	*Impl = FImpl{};
	E_LOG(LogOnline, Display, "Steam 종료");
}

void FSteamSubsystem::RunCallbacks()
{
#if E_WITH_STEAM
	if (Impl->bInitialized)
	{
		SteamAPI_RunCallbacks();
	}
#endif
}

bool FSteamSubsystem::IsAvailable() const
{
	return Impl->bInitialized;
}

uint32 FSteamSubsystem::GetAppId() const
{
	return Impl->AppId;
}

std::string FSteamSubsystem::GetPlayerName() const
{
#if E_WITH_STEAM
	if (Impl->bInitialized)
	{
		return SteamFriends()->GetPersonaName();
	}
#endif
	return {};
}

uint64 FSteamSubsystem::GetSteamId() const
{
#if E_WITH_STEAM
	if (Impl->bInitialized)
	{
		return SteamUser()->GetSteamID().ConvertToUint64();
	}
#endif
	return 0;
}

std::string FSteamSubsystem::GetGameLanguage() const
{
#if E_WITH_STEAM
	if (Impl->bInitialized)
	{
		return SteamApps()->GetCurrentGameLanguage();
	}
#endif
	return {};
}

bool FSteamSubsystem::UnlockAchievement(const std::string& Name)
{
	if (!Impl->bInitialized || Name.empty())
	{
		return false;
	}
#if E_WITH_STEAM
	if (!Impl->bStatsReady)
	{
		Impl->PendingAchievements.push_back(Name); // 통계를 받으면 적용
		return true;
	}
	if (!SteamUserStats()->SetAchievement(Name.c_str()))
	{
		E_LOG(LogOnline, Warning, "업적을 찾을 수 없습니다 (Steamworks에 등록된 API 이름인지 확인): {}", Name);
		return false;
	}
	SteamUserStats()->StoreStats();
	E_LOG(LogOnline, Display, "업적 달성: {}", Name);
	return true;
#else
	return false;
#endif
}

bool FSteamSubsystem::ClearAchievement(const std::string& Name)
{
#if E_WITH_STEAM
	if (Impl->bInitialized && Impl->bStatsReady && SteamUserStats()->ClearAchievement(Name.c_str()))
	{
		SteamUserStats()->StoreStats();
		return true;
	}
#else
	(void)Name;
#endif
	return false;
}

bool FSteamSubsystem::IsAchievementUnlocked(const std::string& Name) const
{
#if E_WITH_STEAM
	bool bAchieved = false;
	if (Impl->bInitialized && Impl->bStatsReady && SteamUserStats()->GetAchievement(Name.c_str(), &bAchieved))
	{
		return bAchieved;
	}
#else
	(void)Name;
#endif
	return false;
}

bool FSteamSubsystem::ActivateOverlay(const std::string& Dialog)
{
#if E_WITH_STEAM
	if (Impl->bInitialized)
	{
		SteamFriends()->ActivateGameOverlay(Dialog.c_str());
		return true;
	}
#else
	(void)Dialog;
#endif
	return false;
}

bool FSteamSubsystem::IsOverlayActive() const
{
	return Impl->bOverlayActive;
}

#include "Scripting/LuaRuntime.h"

#include "Core/Log.h"
#include "Core/Paths.h"

#include <stdexcept>

E_DECLARE_LOG_CATEGORY(LogScript)

// 게임(앱) Lua 바인딩 — 옵션 메뉴/종료 버튼용
//   Game.GetName()                       -- .eproject DisplayName (없으면 Name)
//   Game.GetVersion()                    -- .eproject Version
//   Game.Quit()                          -- 런타임: 이번 프레임 끝에 종료 / 에디터: 플레이 정지
//   Game.GetWindowMode() / Game.SetWindowMode("Windowed" | "BorderlessFullscreen")
//   Game.IsVSync() / Game.SetVSync(true)
// 화면 설정은 런타임이 사용자 설정 파일(<Saved>/Config/GameUserSettings.json)에 저장한다. 앱이 지원하지 않으면 무시(경고 한 번)
//
// Steam (FSteamSubsystem — 런타임이 .eproject SteamAppId로 초기화했을 때만 동작, 아니면 false/빈 값)
//   Steam.IsAvailable() / Steam.GetPlayerName() / Steam.GetLanguage()
//   Steam.UnlockAchievement("ACH_WIN") / Steam.IsAchievementUnlocked("ACH_WIN") / Steam.ClearAchievement("ACH_WIN")
//   Steam.ActivateOverlay("Achievements") / Steam.IsOverlayActive()   -- 오버레이가 떠 있으면 일시정지 권장
void FLuaRuntime::RegisterGameBindings()
{
	sol::table GameTable = Lua.create_named_table("Game");
	GameTable["GetName"] = []() {
		return FPaths::HasProject() ? FPaths::GetProjectDescriptor().GetDisplayName() : std::string("ProjectE");
	};
	GameTable["GetVersion"] = []() {
		return FPaths::HasProject() ? FPaths::GetProjectDescriptor().GetVersion() : std::string("1.0.0");
	};
	GameTable["Quit"] = [this]() {
		if (AppHooks != nullptr && AppHooks->Quit)
		{
			AppHooks->Quit();
		}
		else
		{
			E_LOG(LogScript, Warning, "Game.Quit: 이 앱은 스크립트 종료를 지원하지 않습니다");
		}
	};
	GameTable["GetWindowMode"] = [this]() {
		return AppHooks != nullptr && AppHooks->GetWindowMode ? AppHooks->GetWindowMode() : std::string("Windowed");
	};
	GameTable["SetWindowMode"] = [this](const std::string& Mode) {
		if (AppHooks == nullptr || !AppHooks->SetWindowMode)
		{
			return; // 에디터/서버: 창 모드는 바꾸지 않는다
		}
		if (!AppHooks->SetWindowMode(Mode))
		{
			throw std::runtime_error("Game.SetWindowMode: 알 수 없는 창 모드 \"" + Mode + "\" (Windowed / BorderlessFullscreen)");
		}
	};
	GameTable["IsVSync"] = [this]() {
		return AppHooks == nullptr || !AppHooks->IsVSync || AppHooks->IsVSync();
	};
	GameTable["SetVSync"] = [this](bool bEnabled) {
		if (AppHooks != nullptr && AppHooks->SetVSync)
		{
			AppHooks->SetVSync(bEnabled);
		}
	};

	sol::table SteamTable     = Lua.create_named_table("Steam");
	SteamTable["IsAvailable"] = [this]() { return SteamHooks != nullptr && SteamHooks->IsAvailable && SteamHooks->IsAvailable(); };
	SteamTable["GetPlayerName"] = [this]() {
		return SteamHooks != nullptr && SteamHooks->GetPlayerName ? SteamHooks->GetPlayerName() : std::string();
	};
	SteamTable["GetLanguage"] = [this]() {
		return SteamHooks != nullptr && SteamHooks->GetLanguage ? SteamHooks->GetLanguage() : std::string();
	};
	SteamTable["UnlockAchievement"] = [this](const std::string& Name) {
		return SteamHooks != nullptr && SteamHooks->UnlockAchievement && SteamHooks->UnlockAchievement(Name);
	};
	SteamTable["IsAchievementUnlocked"] = [this](const std::string& Name) {
		return SteamHooks != nullptr && SteamHooks->IsAchievementUnlocked && SteamHooks->IsAchievementUnlocked(Name);
	};
	SteamTable["ClearAchievement"] = [this](const std::string& Name) {
		return SteamHooks != nullptr && SteamHooks->ClearAchievement && SteamHooks->ClearAchievement(Name);
	};
	SteamTable["ActivateOverlay"] = [this](const std::string& Dialog) {
		return SteamHooks != nullptr && SteamHooks->ActivateOverlay && SteamHooks->ActivateOverlay(Dialog);
	};
	SteamTable["IsOverlayActive"] = [this]() { return SteamHooks != nullptr && SteamHooks->IsOverlayActive && SteamHooks->IsOverlayActive(); };
}

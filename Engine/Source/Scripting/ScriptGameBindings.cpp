#include "Scripting/LuaRuntime.h"

#include "Core/Log.h"
#include "Core/Settings/ProjectSettings.h"

#include <stdexcept>

E_DECLARE_LOG_CATEGORY(LogScript)

// 게임(앱) Lua 바인딩 — 옵션 메뉴/종료 버튼용
//   Game.GetName()                       -- .eproject DisplayName (없으면 Name)
//   Game.GetVersion()                    -- .eproject Version
//   Game.Quit()                          -- 런타임: 이번 프레임 끝에 종료 / 에디터: 플레이 정지
//   Game.GetWindowMode() / Game.SetWindowMode("Windowed" | "BorderlessFullscreen")
//   Game.IsVSync() / Game.SetVSync(true)
//   Game.SetMouseLocked(true) / Game.IsMouseLocked()  -- FPS 시점: 커서 숨김 + 창에 가둠 (런타임만, 에디터는 항상 false — 우클릭 시점 등으로 대체)
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
		return FProjectSettings::Get().GetDisplayName();
	};
	GameTable["GetVersion"] = []() {
		return FProjectSettings::Get().Info.Version;
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

	GameTable["SetMouseLocked"] = [this](bool bLocked) {
		if (AppHooks != nullptr && AppHooks->SetMouseLocked)
		{
			AppHooks->SetMouseLocked(bLocked);
		}
	};
	GameTable["IsMouseLocked"] = [this]() { return AppHooks != nullptr && AppHooks->IsMouseLocked && AppHooks->IsMouseLocked(); };

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

	// ---- 맵 전환 (Phase 31)
	//   Game.OpenScene("Scenes/Level2.escene")  -- 또는 Asset("...", ".escene") 값. 서버/Standalone에서만: 이번 프레임 끝에 현재 씬을 바꾼다
	//                                             (모든 스크립트 OnDestroy → 새 씬 로드 → 새 Lua 상태). 접속한 클라이언트도 따라온다.
	//                                             반환: 접수했는가 (클라이언트에서는 경고 후 false). 파일이 없으면 스크립트 오류
	//   Game.GetCurrentScene()                   -- 지금 씬 (Content 기준 경로, 모르면 "")
	//   Game.SetPersistent("Score", 10) / Game.GetPersistent("Score", 0) / Game.ClearPersistent()
	//                                             -- 맵 전환(새 Lua 상태)에도 남는 값: nil/bool/숫자/문자열/Vector3/에셋. 이 프로세스에만 (복제·저장 안 함)
	GameTable["OpenScene"] = [this](const sol::object& Target) {
		std::string SceneAsset;
		if (Target.is<FScriptAssetRef>())
		{
			SceneAsset = Target.as<const FScriptAssetRef&>().Path;
		}
		else if (Target.get_type() == sol::type::string)
		{
			SceneAsset = Target.as<std::string>();
		}
		if (SceneAsset.empty())
		{
			throw std::runtime_error("Game.OpenScene: 씬 경로(문자열 또는 Asset 값)가 필요합니다");
		}
		if (NetHooks == nullptr || !NetHooks->OpenScene)
		{
			throw std::runtime_error("Game.OpenScene: 이 앱은 맵 전환을 지원하지 않습니다");
		}
		const std::string Problem = NetHooks->OpenScene(SceneAsset);
		if (Problem.empty())
		{
			return true;
		}
		if (NetHooks->bIsServer)
		{
			throw std::runtime_error("Game.OpenScene(" + SceneAsset + "): " + Problem); // 파일 없음 등 → 스크립트 오류
		}
		E_LOG(LogScript, Warning, "Game.OpenScene({}): {}", SceneAsset, Problem); // 클라이언트: 서버가 바꾸면 따라간다
		return false;
	};
	GameTable["GetCurrentScene"] = [this]() {
		return NetHooks != nullptr && NetHooks->GetCurrentScene ? NetHooks->GetCurrentScene() : std::string();
	};
	GameTable["SetPersistent"] = [this](const std::string& Key, const sol::object& Value) {
		if (PersistentValues == nullptr)
		{
			return;
		}
		FScriptValue Converted = ToScriptValue(Value);
		if (Converted.IsNil() && Value.valid() && Value.get_type() != sol::type::lua_nil)
		{
			throw std::runtime_error("Game.SetPersistent(" + Key + "): bool/숫자/문자열/Vector3/에셋 값만 넘길 수 있습니다 (테이블은 안 됨)");
		}
		if (Converted.IsNil())
		{
			PersistentValues->erase(Key);
			return;
		}
		(*PersistentValues)[Key] = std::move(Converted);
	};
	GameTable["GetPersistent"] = [this](const std::string& Key, const sol::object& Default) -> sol::object {
		if (PersistentValues != nullptr)
		{
			if (const auto Found = PersistentValues->find(Key); Found != PersistentValues->end())
			{
				return FromScriptValue(Found->second);
			}
		}
		return Default;
	};
	GameTable["ClearPersistent"] = [this]() {
		if (PersistentValues != nullptr)
		{
			PersistentValues->clear();
		}
	};
}

#include "Scripting/LuaRuntime.h"

#include "Core/Console/Console.h"
#include "Core/InputMode.h"
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
//   Game.GetResolutionScale() / Game.SetResolutionScale(0.67)  -- TAAU 화면 비율 (콘솔 변수 r.ScreenPercentage / 100, 0.25~1, 저장하지 않음).
//       Game.IsDynamicResolution() / Game.SetDynamicResolution(true)  -- r.DynamicResolution
//   Game.SetMouseLocked(true) / Game.IsMouseLocked()  -- FPS 시점: 커서 숨김 + 창에 가둠 (에디터는 뷰포트에 빙의 중일 때만, 뷰포트 안에 가둠)
//   Game.SetInputMode("GameOnly" | "GameAndUI" | "UIOnly") / Game.GetInputMode()  -- 입력 모드 (Core/InputMode.h).
//       GameOnly = 게임만 입력(UI는 그리기만, 커서 잠금), GameAndUI = 기본(UI 먼저), UIOnly = UI만(게임은 빈 입력).
//       플레이 시작/정지·맵 전환마다 GameAndUI로 돌아간다. 커서 잠금은 모드에 들어갈 때의 기본값이며 이후 SetMouseLocked로 바꿀 수 있다
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

	// TAAU / 동적 해상도 (Phase 48): 렌더러 콘솔 변수를 직접 (Scripting → Renderer 비의존 — 변수가 없으면 기본값)
	GameTable["GetResolutionScale"] = []() {
		const FConsoleVariable* Var = FConsoleManager::Get().FindVariable("r.ScreenPercentage");
		return Var != nullptr ? Var->GetFloat() / 100.0f : 1.0f;
	};
	GameTable["SetResolutionScale"] = [](float Scale) {
		if (FConsoleVariable* Var = FConsoleManager::Get().FindVariable("r.ScreenPercentage"))
		{
			Var->SetFloat(Scale * 100.0f); // 범위(25~100%)는 변수가 자른다
		}
	};
	GameTable["IsDynamicResolution"] = []() {
		const FConsoleVariable* Var = FConsoleManager::Get().FindVariable("r.DynamicResolution");
		return Var != nullptr && Var->GetBool();
	};
	GameTable["SetDynamicResolution"] = [](bool bEnabled) {
		if (FConsoleVariable* Var = FConsoleManager::Get().FindVariable("r.DynamicResolution"))
		{
			Var->SetBool(bEnabled);
		}
	};
	GameTable["SetMouseLocked"] = [this](bool bLocked) {
		if (AppHooks != nullptr && AppHooks->SetMouseLocked)
		{
			AppHooks->SetMouseLocked(bLocked);
		}
	};
	GameTable["IsMouseLocked"] = [this]() { return AppHooks != nullptr && AppHooks->IsMouseLocked && AppHooks->IsMouseLocked(); };
	GameTable["SetInputMode"]  = [](const std::string& ModeName) {
		EInputMode Mode = EInputMode::GameAndUI;
		if (!TryParseInputMode(ModeName, Mode))
		{
			throw std::runtime_error("Game.SetInputMode: 알 수 없는 입력 모드 \"" + ModeName + "\" (GameOnly / GameAndUI / UIOnly)");
		}
		FInputModeState::Set(Mode);
	};
	GameTable["GetInputMode"] = []() { return std::string(ToString(FInputModeState::Get())); };

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

	// ---- 서브 씬 스트리밍 (Phase 31-2, Scene 테이블). 서버/Standalone에서만 요청 (클라이언트는 서버가 불러온 것을 자동으로 붙인다)
	//   Scene.LoadSubScene("Scenes/Sub.escene", Vector3(0, 0, 0)?) -- 루트 엔티티 아래로 불러온다 (파싱은 백그라운드, 붙이기는 다음 틱). 접수하면 true
	//   Scene.UnloadSubScene("Scenes/Sub.escene")                  -- 루트째 지연 파괴. 불러온/불러오는 중이었으면 true
	//   Scene.IsSubSceneLoaded(path) / Scene.GetSubSceneRoot(path)   -- 붙었는가 / 루트 엔티티 (아직이면 nil)
	//   스크립트 메서드 OnSubSceneLoaded(path)                      -- 붙인 직후 모든 스크립트에 (클라이언트 포함)
	//   볼륨으로 자동: SubSceneVolumeComponent (스트리밍 기준 = 주 카메라, 캐릭터 이동, StreamingSourceComponent)
	const auto ToAssetPath = [](const sol::object& Value) {
		if (Value.is<FScriptAssetRef>())
		{
			return Value.as<const FScriptAssetRef&>().Path;
		}
		return Value.get_type() == sol::type::string ? Value.as<std::string>() : std::string();
	};
	sol::table SceneTable      = Lua["Scene"];
	SceneTable["LoadSubScene"] = [this, ToAssetPath](const sol::object& Target, sol::optional<FVector3> Offset) {
		const std::string Asset = ToAssetPath(Target);
		if (Asset.empty())
		{
			throw std::runtime_error("Scene.LoadSubScene: 씬 경로(문자열 또는 Asset 값)가 필요합니다");
		}
		if (NetHooks == nullptr || !NetHooks->LoadSubScene)
		{
			throw std::runtime_error("Scene.LoadSubScene: 이 앱은 서브 씬을 지원하지 않습니다");
		}
		const std::string Problem = NetHooks->LoadSubScene(Asset, Offset.value_or(FVector3::ZeroVector));
		if (Problem.empty())
		{
			return true;
		}
		if (NetHooks->bIsServer)
		{
			throw std::runtime_error("Scene.LoadSubScene(" + Asset + "): " + Problem);
		}
		E_LOG(LogScript, Warning, "Scene.LoadSubScene({}): {}", Asset, Problem);
		return false;
	};
	SceneTable["UnloadSubScene"] = [this, ToAssetPath](const sol::object& Target) {
		return NetHooks != nullptr && NetHooks->UnloadSubScene && NetHooks->UnloadSubScene(ToAssetPath(Target));
	};
	SceneTable["IsSubSceneLoaded"] = [this, ToAssetPath](const sol::object& Target) {
		return NetHooks != nullptr && NetHooks->IsSubSceneLoaded && NetHooks->IsSubSceneLoaded(ToAssetPath(Target));
	};
	SceneTable["GetSubSceneRoot"] = [this, ToAssetPath](const sol::object& Target) -> sol::object {
		const FEntity Root = NetHooks != nullptr && NetHooks->GetSubSceneRoot ? NetHooks->GetSubSceneRoot(ToAssetPath(Target)) : NullEntity;
		return Root.IsValid() ? sol::make_object(Lua, FScriptEntity{ Root }) : sol::object(sol::lua_nil);
	};
}

#include "Core/Settings/ProjectSettings.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Project.h"
#include "Core/Settings/SettingsRegistry.h"

namespace
{
	constexpr const char* GProjectCategory = "프로젝트";
	constexpr const char* GEngineCategory  = "엔진";
} // namespace

FProjectSettings& FProjectSettings::Get()
{
	static FProjectSettings Settings; // 엔진 DLL 안 하나
	return Settings;
}

FProjectSettings::FProjectSettings()
{
	FSettingsRegistry& Registry = FSettingsRegistry::Get();

	Registry.Register(Info, { "Project", "프로젝트 정보", GProjectCategory, "패키지 실행 파일의 이름/아이콘/버전 정보, 창 제목, Steam" })
		.Property(&FProjectInfoSettings::DisplayName, "DisplayName", "표시 이름").Tooltip("창 제목과 실행 파일 제품 이름. 비면 프로젝트 이름")
		.Property(&FProjectInfoSettings::Version, "Version", "버전").Tooltip("\"주.부.수[.빌드]\" (각 0~65535) — 실행 파일 버전 정보")
		.Property(&FProjectInfoSettings::Company, "Company", "회사").Tooltip("실행 파일 회사 이름, 패키지 게임의 사용자 저장 폴더(%LOCALAPPDATA%\\<회사>\\<프로젝트>)")
		.Property(&FProjectInfoSettings::Icon, "Icon", "아이콘").Tooltip("프로젝트 폴더 기준 .ico 또는 .png (정사각형 256px 권장)")
		.Property(&FProjectInfoSettings::ExecutableName, "ExecutableName", "실행 파일 이름").Tooltip("패키지 exe 이름 (확장자 제외). 비면 프로젝트 이름. 게임 모듈 이름과 같으면 안 된다")
		.Property(&FProjectInfoSettings::SteamAppId, "SteamAppId", "Steam App ID").Tooltip("0이면 Steam을 쓰지 않는다. 런타임만 초기화")
		.Property(&FProjectInfoSettings::SteamDepotId, "SteamDepotId", "Steam Depot ID").Tooltip("SteamUpload.ps1이 올릴 디포. 0이면 App ID + 1");

	Registry.Register(Maps, { "Maps", "맵 & 모드", GProjectCategory, "시작 맵과 플레이어 프리팹 (Content 기준 경로)" })
		.Property(&FMapsSettings::EditorStartupMap, "EditorStartupMap", "에디터 시작 맵").AssetFilter(".escene")
		.Tooltip("에디터를 열 때 여는 씬. 비면 게임 기본 맵 (에디터 환경설정 '시작 시 마지막 씬 열기'가 우선)")
		.Property(&FMapsSettings::GameDefaultMap, "GameDefaultMap", "게임 기본 맵").AssetFilter(".escene").Tooltip("런타임/패키지 게임이 시작할 때 여는 씬")
		.Property(&FMapsSettings::ServerDefaultMap, "ServerDefaultMap", "서버 기본 맵").AssetFilter(".escene").Tooltip("전용 서버 시작 씬. 비면 게임 기본 맵")
		.Property(&FMapsSettings::PlayerPrefab, "PlayerPrefab", "플레이어 프리팹").AssetFilter(".eprefab").Tooltip("멀티플레이: 플레이어가 입장하면 서버가 PlayerStart 위치에 만든다");

	Registry.Register(Packaging, { "Packaging", "패키징", GProjectCategory, "Scripts\\Package.ps1의 기본값 (명령줄 인자가 우선)" })
		.Property(&FPackagingSettings::Configuration, "Configuration", "빌드 구성").Enum({ { "Release", "Release (배포)" }, { "Debug", "Debug (검증용, 배포 불가)" } })
		.Property(&FPackagingSettings::bUsePak, "UsePak", "pak으로 묶기").Tooltip("콘텐츠를 Content.epak 하나로 묶는다")
		.Property(&FPackagingSettings::bIncludeSourceAssets, "IncludeSourceAssets", "원본 포함").Tooltip("원본 모델/이미지, 셰이더 소스, DXC까지 넣는다 (디버깅용, 용량 증가)")
		.Property(&FPackagingSettings::AdditionalDirectories, "AdditionalDirectories", "추가 폴더").Tooltip("프로젝트 폴더 기준, \";\"로 구분. pak에 넣지 않고 파일로 복사한다");

	Registry.Register(Physics, { "Physics", "물리", GEngineCategory, "다음 플레이(물리 시작)부터 적용" })
		.Property(&FPhysicsSettings::Gravity, "Gravity", "중력 (cm/s²)")
		.Property(&FPhysicsSettings::FixedStepHz, "FixedStepHz", "고정 스텝 (Hz)").Range(15.0f, 240.0f, 1.0f).Tooltip("물리 시뮬레이션 빈도. 높을수록 정확하지만 비싸다")
		.Property(&FPhysicsSettings::MaxSubSteps, "MaxSubSteps", "최대 서브스텝").Range(1.0f, 16.0f).Tooltip("느린 프레임에서 한 번에 따라잡는 물리 스텝 상한");

	Registry.Register(Network, { "Network", "네트워크", GEngineCategory, "다음 세션부터 적용. 명령줄 --port가 우선" })
		.Property(&FNetworkSettings::DefaultPort, "DefaultPort", "기본 포트").Range(1024.0f, 65535.0f)
		.Property(&FNetworkSettings::LanDiscoveryPort, "LanDiscoveryPort", "LAN 검색 포트").Range(1024.0f, 65535.0f).Tooltip("UDP. 같은 LAN의 방 목록 찾기")
		.Property(&FNetworkSettings::MaxPlayers, "MaxPlayers", "최대 인원").Range(1.0f, 64.0f).Tooltip("서버의 원격 플레이어 최대 수 (리슨 서버 호스트 제외)")
		.Property(&FNetworkSettings::bClientPrediction, "ClientPrediction", "클라이언트 예측")
		.Tooltip("캐릭터를 소유 클라이언트가 입력 즉시 미리 움직인다. 끄면 모든 캐릭터가 서버 결과를 보간해 보여 준다 (캐릭터 이동 컴포넌트의 클라이언트 예측도 켜져 있어야 예측한다)")
		.Property(&FNetworkSettings::bPhysicsPrediction, "PhysicsPrediction", "물리 예측")
		.Tooltip("내 캐릭터 근처나 닿은 복제 물리 물체(상자/공)를 클라이언트가 미리 시뮬레이션하고 서버 상태로 부드럽게 수렴시킨다. 끄면 서버 결과를 보간해 보여 준다 (밀면 늦게 반응)")
		.Property(&FNetworkSettings::PhysicsPredictionRadius, "PhysicsPredictionRadius", "물리 예측 반경 (cm)").Range(50.0f, 5000.0f, 10.0f)
		.Tooltip("예측 캐릭터 중심에서 이 거리 안에 중심이 있는 물체를 예측한다 (닿은 물체는 거리와 무관)");

	Registry.Register(Display, { "Display", "화면 기본값", GEngineCategory, "게임의 처음 화면 설정. 플레이어가 바꾼 값(<Saved>/Config/GameUserSettings.json)이 우선" })
		.Property(&FGameUserSettings::WindowMode, "WindowMode", "창 모드").Enum({ { "Windowed", "창 모드" }, { "BorderlessFullscreen", "테두리 없는 전체 화면" } })
		.Property(&FGameUserSettings::WindowWidth, "WindowWidth", "창 너비").Range(320.0f, 7680.0f)
		.Property(&FGameUserSettings::WindowHeight, "WindowHeight", "창 높이").Range(240.0f, 7680.0f)
		.Property(&FGameUserSettings::bVSync, "VSync", "수직 동기화");

	// 액션 목록은 리플렉션 값 타입이 아니므로 사용자 정의 JSON 섹션 (설정 창은 전용 편집 UI — Editor/Panels/InputSettingsEditor)
	Registry.RegisterCustom(
		&Input, { "Input", "입력", GEngineCategory, "입력 액션(버튼/1D/2D 축)과 바인딩(키, 마우스, 게임패드). 플레이어 재지정은 <Saved>/Config/InputBindings.json" },
		[this](std::string_view Json, std::string* Error) {
			FInputMapping Mapping;
			if (!FInputMapping::FromJson(Json, Mapping, Error))
			{
				return false;
			}
			if (Error != nullptr && !Error->empty())
			{
				E_LOG(LogCore, Warning, "입력 설정 일부를 건너뜀: {}", *Error);
				Error->clear();
			}
			Input.SetProjectMapping(std::move(Mapping));
			return true;
		},
		[this]() { return Input.GetProjectMapping().ToJson(); }, [this]() { Input.ResetProjectMapping(); });

	Registry.Register(Localization, { "Localization", "다국어", GProjectCategory, "문자열 표와 언어. 플레이어가 고른 언어는 <Saved>/Config/Language.json" })
		.Property(&FLocalizationSettings::DefaultLanguage, "DefaultLanguage", "기본 언어").Tooltip("언어 코드 (ko, en, ja ...). 현재 언어에 없는 문자열은 이 언어로 보여 준다")
		.Property(&FLocalizationSettings::StringTables, "StringTables", "문자열 표")
		.Tooltip("Content 기준 .estrings 경로 (\";\" 구분). 비면 Content/Localization 폴더의 모든 .estrings (패키지 pak에서는 폴더를 뒤질 수 없으므로 지정 권장)")
		.Property(&FLocalizationSettings::bDetectSystemLanguage, "DetectSystemLanguage", "시스템 언어 자동 선택")
		.Tooltip("플레이어가 언어를 고른 적이 없으면 Windows 표시 언어가 표에 있을 때 그 언어로 시작한다");
}

void FProjectSettings::ResetToDefaults()
{
	Info      = {};
	Maps      = {};
	Packaging = {};
	Physics   = {};
	Network   = {};
	Display   = {};
	Localization = {};
	Input.ResetProjectMapping(); // 사용자 재지정은 유지 (플레이어 파일)
}

void FProjectSettings::LoadForProject(const FProjectDescriptor& Descriptor)
{
	ResetToDefaults();
	ProjectName = Descriptor.Name;

	// 마이그레이션: 예전 .eproject에 있던 값 (Config 파일이 있으면 그쪽이 덮어쓴다)
	const FProjectLegacyFields& Legacy = Descriptor.Legacy;
	if (!Legacy.DefaultScene.empty())
	{
		Maps.GameDefaultMap = Legacy.DefaultScene;
	}
	Maps.PlayerPrefab    = Legacy.PlayerPrefab;
	Info.DisplayName     = Legacy.DisplayName;
	Info.Company         = Legacy.Company;
	Info.Icon            = Legacy.Icon;
	Info.ExecutableName  = Legacy.ExecutableName;
	Info.SteamAppId      = Legacy.SteamAppId;
	if (!Legacy.Version.empty())
	{
		Info.Version = Legacy.Version;
	}

	// Display.json이 없으면 예전 이름(DefaultGameUserSettings.json)
	const FSettingsSection* DisplaySection = FSettingsRegistry::Get().Find("Display");
	if (DisplaySection != nullptr && !std::filesystem::exists(DisplaySection->GetFilePath()))
	{
		Display.ApplyFile(FPaths::GetProjectConfigDirectory() / L"DefaultGameUserSettings.json");
	}
	FSettingsRegistry::Get().LoadAll(ESettingsScope::Project);
}

bool FProjectSettings::SaveAll() const
{
	bool bOk = true;
	for (const std::unique_ptr<FSettingsSection>& Section : FSettingsRegistry::Get().GetSections())
	{
		if (Section->Scope == ESettingsScope::Project)
		{
			bOk &= Section->Save();
		}
	}
	return bOk;
}

std::string FProjectSettings::GetDisplayName() const
{
	if (!Info.DisplayName.empty())
	{
		return Info.DisplayName;
	}
	return ProjectName.empty() ? std::string("ProjectE") : ProjectName;
}

std::string FProjectSettings::GetExecutableName() const
{
	if (!Info.ExecutableName.empty())
	{
		return Info.ExecutableName;
	}
	return ProjectName.empty() ? std::string("ProjectE") : ProjectName;
}

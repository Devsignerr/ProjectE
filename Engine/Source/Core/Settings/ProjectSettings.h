#pragma once

#include "Core/CoreTypes.h"
#include "Core/GameUserSettings.h"
#include "Core/Settings/InputSettings.h"
#include "Core/Math/Math.h"

#include <string>
#include <string_view>

struct FProjectDescriptor;

// ---- 프로젝트 설정 섹션 (각각 <프로젝트>/Config/<Id>.json, 저장소에 커밋). 새 항목은 필드 + ProjectSettings.cpp 등록만 하면 창/저장에 나타난다

// "Project" — 프로젝트 정보 (패키징 exe 리소스, 창 제목, Steam)
struct FProjectInfoSettings
{
	std::string DisplayName;     // 비면 프로젝트 이름
	std::string Version = "1.0.0";
	std::string Company;         // exe 회사 이름, 패키지 사용자 저장 폴더 상위
	std::string Icon;            // 프로젝트 폴더 기준 .ico/.png
	std::string ExecutableName;  // 패키지 exe 이름 (확장자 제외, 비면 프로젝트 이름)
	uint32      SteamAppId   = 0; // 0 = Steam 안 씀
	uint32      SteamDepotId = 0; // 0 = App ID + 1
};

// "Maps" — 맵 & 모드 (Content 기준 경로)
struct FMapsSettings
{
	std::string EditorStartupMap; // 에디터 시작 맵 (비면 GameDefaultMap)
	std::string GameDefaultMap;   // 런타임/패키지 게임 시작 맵
	std::string ServerDefaultMap; // 전용 서버 시작 맵 (비면 GameDefaultMap)
	std::string PlayerPrefab;     // 멀티플레이: 입장한 플레이어마다 서버가 만드는 프리팹
};

enum class EPackageConfiguration : int32
{
	Release,
	Debug,
};

// "Packaging" — Scripts/Package.ps1이 읽는다 (명령줄 인자가 우선)
struct FPackagingSettings
{
	EPackageConfiguration Configuration        = EPackageConfiguration::Release;
	bool                  bUsePak              = true;
	bool                  bIncludeSourceAssets = false; // 원본 모델/이미지·셰이더 소스·DXC까지 (디버깅용)
	std::string           AdditionalDirectories;        // 프로젝트 폴더 기준 추가 복사 폴더 (";" 구분, 파일로 둔다)
};

// "Physics" — 다음 플레이(물리 시작)부터 적용
struct FPhysicsSettings
{
	FVector3 Gravity     = FVector3(0.0f, 0.0f, -980.665f); // cm/s²
	float    FixedStepHz = 60.0f;
	uint32   MaxSubSteps = 4; // 한 프레임 최대 물리 스텝 (느린 프레임에서 따라잡기 상한)
};

// "Network" — 다음 세션부터 적용
struct FNetworkSettings
{
	uint32 DefaultPort      = 7777;
	uint32 LanDiscoveryPort = 7778;
	uint32 MaxPlayers       = 16; // 서버의 원격 플레이어 최대 수
	bool   bClientPrediction = true; // 캐릭터 클라이언트 예측 (끄면 캐릭터별 설정과 무관하게 모두 끈다 — 비교/디버깅)
	bool   bPhysicsPrediction = true;          // 물리 예측: 예측 캐릭터 근처/접촉한 복제 동적 바디를 클라이언트가 로컬로 시뮬레이션 (캐릭터 예측이 켜져 있어야)
	float  PhysicsPredictionRadius = 300.0f;   // cm, 예측 캐릭터 중심에서 이 거리 안의 바디 중심이면 예측 대상
};

// "Localization" — 다국어 (UI 모듈 FLocalization이 처음 쓸 때 읽는다)
struct FLocalizationSettings
{
	std::string DefaultLanguage = "ko";        // 현재 언어에 없는 문자열의 대체 언어, 처음 실행 언어
	std::string StringTables;                  // Content 기준 .estrings (";" 구분). 비면 디스크의 Content/Localization/*.estrings
	bool        bDetectSystemLanguage = false; // 사용자 설정이 없을 때 시스템 언어가 표에 있으면 그 언어
};

// "Console" — 개발자 콘솔 (런타임 ` 키 오버레이, Core/Console). 개발 실행은 항상 켜짐
struct FConsoleSettings
{
	bool bEnableInPackagedGame = false; // 패키지 게임에서도 ` 키 콘솔 (치트 변수는 여전히 막힘)
};

// 프로젝트 설정 전체 (엔진 DLL 전역 하나). FPaths가 프로젝트를 열 때 LoadForProject를 부른다.
// "Display" 섹션은 FGameUserSettings(창 모드/해상도/VSync)의 프로젝트 기본값이다 — 사용자 설정 파일이 그 위에 덮인다.
class FProjectSettings
{
public:
	static FProjectSettings& Get();

	FProjectInfoSettings Info;
	FMapsSettings        Maps;
	FPackagingSettings   Packaging;
	FPhysicsSettings     Physics;
	FNetworkSettings     Network;
	FGameUserSettings    Display;
	FInputSettings       Input; // "Input" — 입력 액션/바인딩 (Config/Input.json, 사용자 재지정 포함)
	FLocalizationSettings Localization;
	FConsoleSettings      Console;

	// 기본값 → .eproject의 이전 필드(DefaultScene 등, 마이그레이션) → Config/<Id>.json 순서로 채운다
	void LoadForProject(const FProjectDescriptor& Descriptor);
	void ResetToDefaults();
	bool SaveAll() const; // 모든 프로젝트 섹션을 Config/에 쓴다

	// ---- 대체값 규칙
	std::string GetDisplayName() const;
	std::string GetExecutableName() const;
	std::string GetEditorStartupMap() const { return Maps.EditorStartupMap.empty() ? Maps.GameDefaultMap : Maps.EditorStartupMap; }
	std::string GetServerDefaultMap() const { return Maps.ServerDefaultMap.empty() ? Maps.GameDefaultMap : Maps.ServerDefaultMap; }
	uint32      GetSteamDepotId() const { return Info.SteamDepotId != 0 ? Info.SteamDepotId : (Info.SteamAppId != 0 ? Info.SteamAppId + 1 : 0); }

	FProjectSettings(const FProjectSettings&)            = delete;
	FProjectSettings& operator=(const FProjectSettings&) = delete;

private:
	FProjectSettings(); // 섹션 등록
	std::string ProjectName;
};

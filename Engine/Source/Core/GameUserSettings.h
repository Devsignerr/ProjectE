#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>
#include <string_view>

enum class EWindowMode : int32 // 4바이트: 리플렉션 enum 프로퍼티 (설정 창)
{
	Windowed,
	BorderlessFullscreen, // 모니터 전체를 덮는 테두리 없는 창 (전용 전체 화면은 쓰지 않는다 — flip 모델)
};

// 화면 해상도 품질 프리셋 (TAAU 화면 비율, Phase 48): 네이티브 100% / 품질 77% / 균형 67% / 성능 50% (FUpscaleMath::GetPresetScreenPercentage)
enum class EResolutionQuality : int32 // 4바이트: 리플렉션 enum 프로퍼티
{
	Native,
	Quality,
	Balanced,
	Performance,
};

// HDR 디스플레이 출력 (Phase 49): 끔(SDR) / 자동(디스플레이가 HDR이면 HDR10) / HDR10(PQ, BT.2020) / scRGB(선형 FP16)
enum class EHdrOutputMode : int32 // 4바이트: 리플렉션 enum 프로퍼티
{
	Off,
	Auto,
	Hdr10,
	ScRgb,
};

const char* ToString(EWindowMode Mode);
const char* ToString(EHdrOutputMode Mode);
bool        TryParseHdrOutputMode(std::string_view Text, EHdrOutputMode& OutMode); // "Off" / "Auto" / "Hdr10" / "ScRgb"
const char* ToString(EResolutionQuality Quality);
bool        TryParseResolutionQuality(std::string_view Text, EResolutionQuality& OutQuality); // "Native" / "Quality" / "Balanced" / "Performance"
bool        TryParseWindowMode(std::string_view Text, EWindowMode& OutMode); // "Windowed" / "BorderlessFullscreen" (대소문자 무시)

// 게임 사용자 설정 (화면). 런타임이 시작할 때 읽고, 바뀌면 저장한다.
//   프로젝트 설정 "Display" 섹션(Config/Display.json, FProjectSettings::Display) ← <Saved>/Config/GameUserSettings.json(사용자)
// JSON: { "WindowMode": "Windowed", "WindowWidth": 1280, "WindowHeight": 720, "VSync": true, "ResolutionQuality": "Native",
//         "DynamicResolution": false, "DynamicResolutionTargetMs": 16.6, "HdrOutput": "Off", "HdrPaperWhiteNits": 200, "HdrMaxNits": 0 }
// 파일에 없는 키는 앞 단계 값을 유지한다.
struct FGameUserSettings
{
	EWindowMode WindowMode   = EWindowMode::Windowed;
	uint32      WindowWidth  = 1280; // 창 모드 클라이언트 크기
	uint32      WindowHeight = 720;
	bool        bVSync       = true;
	// TAAU (Phase 48): 씬 렌더 해상도 프리셋 + 동적 해상도 (목표 GPU 씬 렌더 시간). 런타임이 시작할 때 r.ScreenPercentage 등에 적용
	EResolutionQuality ResolutionQuality         = EResolutionQuality::Native;
	bool               bDynamicResolution        = false;
	float              DynamicResolutionTargetMs = 16.6f;
	// HDR 출력 (Phase 49): 디스플레이가 지원하지 않으면 SDR 유지. 종이 흰색 = SDR/UI 흰색의 밝기, 최대 밝기 0 = 디스플레이 값
	EHdrOutputMode     HdrOutput         = EHdrOutputMode::Off;
	float              HdrPaperWhiteNits = 200.0f;
	float              HdrMaxNits        = 0.0f;

	// JSON 문자열을 현재 값 위에 덮어쓴다. JSON 오류면 false (값은 그대로)
	bool        ApplyJson(std::string_view Json);
	std::string ToJson() const;

	// 파일이 없으면 true (그대로), 읽기/JSON 오류면 경고 후 false
	bool ApplyFile(const std::filesystem::path& Path);
	bool SaveToFile(const std::filesystem::path& Path) const;

	// 프로젝트 기본값 + 사용자 설정 (FPaths 초기화 후)
	static FGameUserSettings     Load();
	static std::filesystem::path GetUserSettingsPath(); // <Saved>/Config/GameUserSettings.json
	bool                         Save() const;          // GetUserSettingsPath()에
};

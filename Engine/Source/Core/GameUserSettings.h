#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>
#include <string_view>

enum class EWindowMode : uint8
{
	Windowed,
	BorderlessFullscreen, // 모니터 전체를 덮는 테두리 없는 창 (전용 전체 화면은 쓰지 않는다 — flip 모델)
};

const char* ToString(EWindowMode Mode);
bool        TryParseWindowMode(std::string_view Text, EWindowMode& OutMode); // "Windowed" / "BorderlessFullscreen" (대소문자 무시)

// 게임 사용자 설정 (화면). 런타임이 시작할 때 읽고, 바뀌면 저장한다.
//   기본값 ← 프로젝트 Config/DefaultGameUserSettings.json(선택) ← <Saved>/Config/GameUserSettings.json(사용자)
// JSON: { "WindowMode": "Windowed", "WindowWidth": 1280, "WindowHeight": 720, "VSync": true }
// 파일에 없는 키는 앞 단계 값을 유지한다.
struct FGameUserSettings
{
	EWindowMode WindowMode   = EWindowMode::Windowed;
	uint32      WindowWidth  = 1280; // 창 모드 클라이언트 크기
	uint32      WindowHeight = 720;
	bool        bVSync       = true;

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

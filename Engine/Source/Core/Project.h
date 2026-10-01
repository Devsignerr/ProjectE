#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>

// 예전 .eproject에 있던 설정 (Phase 20 이전). 읽기만 하고 쓰지 않는다 — FProjectSettings가 로드할 때 Config/로 옮겨 읽는다
struct FProjectLegacyFields
{
	std::string DefaultScene;
	std::string PlayerPrefab;
	std::string DisplayName;
	std::string Version;
	std::string Company;
	std::string ExecutableName;
	std::string Icon;
	uint32      SteamAppId = 0;

	bool IsEmpty() const;
};

// .eproject 파일 내용 (JSON) — 프로젝트의 정체성만. 설정은 Config/<섹션>.json (FProjectSettings)
//   { "Name": "Sample", "EngineVersion": "0.1.0", "GameModule": "SampleGame" }
struct FProjectDescriptor
{
	static constexpr const wchar_t* Extension = L".eproject";

	std::string Name;
	std::string EngineVersion = "0.1.0";
	std::string GameModule; // 게임 모듈 DLL 이름 (확장자 제외, 실행 파일 폴더에서 찾는다). 비어 있으면 없음

	FProjectLegacyFields Legacy; // 마이그레이션용 (저장하지 않음)

	// 실패 시 false (파일 없음/JSON 오류는 Error 로그). 성공 시 필드가 덮어써진다.
	bool LoadFromFile(const std::filesystem::path& Path);

	// 상위 디렉터리가 없으면 생성한다. 이전 필드는 쓰지 않는다
	bool SaveToFile(const std::filesystem::path& Path) const;
};

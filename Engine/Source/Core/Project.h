#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>

// .eproject 파일 내용 (JSON)
//   { "Name": "Sample", "EngineVersion": "0.1.0", "DefaultScene": "Scenes/Main.escene" }
struct FProjectDescriptor
{
	static constexpr const wchar_t* Extension = L".eproject";

	std::string Name;
	std::string EngineVersion = "0.1.0";
	std::string DefaultScene; // 프로젝트 Content 기준 상대 경로. 비어 있을 수 있음

	// 실패 시 false (파일 없음/JSON 오류는 Error 로그). 성공 시 필드가 덮어써진다.
	bool LoadFromFile(const std::filesystem::path& Path);

	// 상위 디렉터리가 없으면 생성한다
	bool SaveToFile(const std::filesystem::path& Path) const;
};

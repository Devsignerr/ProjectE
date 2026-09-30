#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <string>

// .eproject 파일 내용 (JSON)
//   { "Name": "Sample", "EngineVersion": "0.1.0", "DefaultScene": "Scenes/Main.escene", "GameModule": "SampleGame",
//     "PlayerPrefab": "Prefabs/Player.eprefab",
//     "DisplayName": "샘플 게임", "Version": "1.0.0", "Company": "MyStudio", "ExecutableName": "SampleGame", "Icon": "Build/Icon.png" }
struct FProjectDescriptor
{
	static constexpr const wchar_t* Extension = L".eproject";

	std::string Name;
	std::string EngineVersion = "0.1.0";
	std::string DefaultScene; // 프로젝트 Content 기준 상대 경로. 비어 있을 수 있음
	std::string GameModule;   // 게임 모듈 DLL 이름 (확장자 제외, 실행 파일 폴더에서 찾는다). 비어 있으면 없음
	std::string PlayerPrefab; // 멀티플레이: 플레이어가 입장하면 서버가 만드는 프리팹 (Content 기준). 비어 있으면 만들지 않는다

	// ---- 배포 (패키징/실행 파일 정보). 비어 있으면 Get* 함수가 대체값을 준다
	std::string DisplayName;    // 창 제목·exe 제품 이름
	std::string Version;        // 게임 버전 "주.부.수[.빌드]"
	std::string Company;        // exe 회사 이름, 패키지 사용자 저장 폴더 상위 이름
	std::string ExecutableName; // 패키지 exe 이름 (확장자 제외)
	std::string Icon;           // 프로젝트 폴더 기준 .ico 또는 .png

	const std::string& GetDisplayName() const { return DisplayName.empty() ? Name : DisplayName; }
	const std::string& GetExecutableName() const { return ExecutableName.empty() ? Name : ExecutableName; }
	std::string        GetVersion() const { return Version.empty() ? std::string("1.0.0") : Version; }

	// 실패 시 false (파일 없음/JSON 오류는 Error 로그). 성공 시 필드가 덮어써진다.
	bool LoadFromFile(const std::filesystem::path& Path);

	// 상위 디렉터리가 없으면 생성한다
	bool SaveToFile(const std::filesystem::path& Path) const;
};

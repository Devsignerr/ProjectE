#pragma once

#include "Core/CoreTypes.h"
#include "Core/Project.h"

#include <filesystem>
#include <string>

class FCommandLine;

// 엔진/프로젝트 경로 체계 (컴파일 타임 절대 경로 대신 이것만 사용한다)
//
//   엔진 디렉터리: 실행 파일 디렉터리에서 상위로 올라가며 "Engine/Shaders/Shaders.json" 마커가 있는 첫 디렉터리.
//                  개발 빌드(Build/ninja-debug/Bin)와 패키징(exe 옆에 Engine/) 모두 만족한다.
//   프로젝트:      .eproject 파일이 있는 폴더. Content/ (에셋), Saved/ (런타임 생성물), Config/ (설정).
//                  명령줄 "--project <파일|폴더>" 우선, 없으면 <실행 파일 폴더>/<실행 파일 이름>/ (패키지 배치),
//                  그것도 없으면 개발 편의로 엔진의 Projects/Sample/Sample.eproject.
//   패키지:        엔진 디렉터리에 Engine/Packaged.json(Package.ps1이 기록)이 있으면 패키징된 게임이다.
//                  패키지의 Saved는 설치 폴더가 아니라 %LOCALAPPDATA%/[<Company>/]<프로젝트>/Saved (쓰기 권한).
class FPaths
{
public:
	// 프로세스당 1회. 프로세스 명령줄을 사용한다.
	static void Initialize();
	static void Initialize(const FCommandLine& CommandLine);
	static bool IsInitialized();

	// ---- 엔진
	static const std::filesystem::path& GetExecutableDirectory();
	static const std::filesystem::path& GetEngineDirectory();
	static std::filesystem::path        GetEngineShaderDirectory(); // Engine/Shaders
	static bool                         IsPackaged();                // Engine/Packaged.json 존재

	// ---- 프로젝트
	// .eproject 파일 또는 그 파일이 하나 들어 있는 폴더. 실패 시 false이고 기존 프로젝트는 유지된다.
	static bool SetProject(const std::filesystem::path& ProjectFileOrDirectory);
	static bool HasProject();

	static const std::string&            GetProjectName();
	static const FProjectDescriptor&     GetProjectDescriptor();
	static const std::filesystem::path&  GetProjectFile();
	static std::filesystem::path         GetProjectDirectory();
	static std::filesystem::path         GetProjectContentDirectory(); // <프로젝트>/Content
	static std::filesystem::path         GetProjectConfigDirectory();  // <프로젝트>/Config
	static std::filesystem::path         GetProjectSavedDirectory();   // <프로젝트>/Saved, 패키지는 사용자 폴더 (없으면 생성)

	// 저장 폴더: 프로젝트가 있으면 프로젝트 Saved, 없으면 <엔진>/Saved (없으면 생성)
	static std::filesystem::path GetSavedDirectory();
	static std::filesystem::path GetLogDirectory();   // <Saved>/Logs (없으면 생성)
	static std::filesystem::path GetCrashDirectory(); // <Saved>/Crashes (없으면 생성)

	// ---- 유틸 (테스트/도구용)
	// StartDirectory에서 상위로 올라가며 엔진 마커를 찾는다. 없으면 빈 경로
	static std::filesystem::path FindEngineDirectory(const std::filesystem::path& StartDirectory);
	// 폴더가 주어지면 안의 *.eproject 하나를 찾는다. 파일이면 그대로. 없으면 빈 경로
	static std::filesystem::path ResolveProjectFile(const std::filesystem::path& FileOrDirectory);
	// 패키지 사용자 저장 폴더: <LocalAppData>/[<Company>/]<ProjectName>/Saved. 파일 이름에 못 쓰는 문자는 '_'로 바꾼다
	static std::filesystem::path MakeUserSavedDirectory(const std::filesystem::path& LocalAppData, const std::string& Company,
	                                                    const std::string& ProjectName);
	static std::string           SanitizeFileName(const std::string& Name);
};

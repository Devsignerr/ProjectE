#pragma once

#include "Scene/GameModule.h"

#include <filesystem>
#include <string>

// 게임 모듈 DLL 로드/수명 관리.
//   Load:   DLL 로드 → 버전 확인 → 모듈 생성 → (등록 소유자 = 모듈 이름으로) OnLoad
//   Unload: OnUnload → 모듈이 등록한 리플렉션 타입 제거. DLL은 프로세스 종료까지 내리지 않는다
//           (씬의 게임 컴포넌트 풀 등이 모듈 코드를 참조할 수 있으므로. 핫 리로드는 후속)
class FGameModuleHost
{
public:
	~FGameModuleHost();

	// 실행 파일 폴더의 <Name>.dll
	static std::filesystem::path GetDefaultModulePath(const std::string& Name);

	bool Load(const std::filesystem::path& DllPath);
	void Unload();
	bool IsLoaded() const { return Module != nullptr; }
	const std::string& GetName() const { return Name; }

	// 로드되지 않았으면 아무것도 하지 않는다
	void BeginPlay(FScene& Scene);
	void Update(FScene& Scene, float DeltaSeconds);
	void EndPlay(FScene& Scene);
	bool IsPlaying() const { return bPlaying; }

private:
	void*        Library = nullptr; // HMODULE (공개 헤더에 Windows.h 금지)
	IGameModule* Module  = nullptr; // 모듈 DLL 안의 정적 인스턴스 (비소유)
	std::string  Name;
	bool         bPlaying = false;
};

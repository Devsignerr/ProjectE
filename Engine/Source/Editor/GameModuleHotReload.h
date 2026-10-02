#pragma once

#include "Core/CoreTypes.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// C++ 게임 모듈 핫 리로드 (에디터 전용, Phase 48 사이드).
//
// 그림자 복사: 에디터는 원본 <Bin>/<이름>.dll을 직접 로드하지 않고 <Saved>/HotReload/<이름>_<번호>.dll(+ 같은 이름 PDB)
//   복사본을 로드한다 → 원본 DLL/PDB가 잠기지 않아 링커가 덮어쓸 수 있다. 복사본 DLL의 디버그 디렉터리(CodeView RSDS)의
//   PDB 경로는 복사본 PDB 파일 이름("<이름>_<번호>.pdb", 폴더 없음)으로 바꾼다 → 디버거·dbghelp(크래시 핸들러, Tracy)가
//   복사본 폴더의 복사본 PDB를 읽고 원본 PDB를 열지 않는다(원본 PDB를 디버거가 잡아 다음 링크가 실패하는 문제 방지).
//   런타임/서버/패키지 게임은 지금처럼 원본을 직접 로드한다(이 파일은 에디터만 쓴다).
// 다시 로드 절차는 FEditorApplication::ReloadGameModule (Editor/EditorHotReload.cpp) 머리 주석이 기준.
// 이전 복사본 DLL은 FreeLibrary하지 않으므로 그 파일은 프로세스가 끝날 때까지 지울 수 없다 — 다음 에디터 시작 때 정리한다.

namespace GameModuleHotReload
{
	// "<Name>_<Number>.dll" 형식 파일 이름 중 가장 큰 번호 + 1 (없으면 1). 대소문자 무시, 다른 이름/형식은 무시
	uint32 FindNextShadowCopyNumber(const std::vector<std::wstring>& ExistingFileNames, const std::string& Name);
	std::filesystem::path MakeShadowCopyPath(const std::filesystem::path& Directory, const std::string& Name, uint32 Number); // .dll

	// PE 이미지(파일 바이트)의 CodeView(RSDS) PDB 경로 읽기/바꾸기. 바꾸기는 원래 문자열 칸 안에서만(남는 칸은 0) — 넘치면 false
	bool ReadPdbPath(const std::vector<uint8>& Image, std::string& OutPath);
	bool PatchPdbPath(std::vector<uint8>& Image, const std::string& NewPdbPath);
} // namespace GameModuleHotReload

// 파일 변경 안정화: 관찰한 (시각, 크기)가 기준과 달라지면 대기를 시작하고, StableSeconds 동안 값이 그대로면 한 번 "준비됨".
// 링커가 DLL을 여러 번에 나눠 쓰는 동안 다시 로드하지 않도록 한다 (순수 로직 — 시각은 호출자가 준다)
class FFileChangeDebouncer
{
public:
	struct FStamp
	{
		int64  WriteTime = 0;
		uint64 Size      = 0;
		bool   bExists   = false;

		bool operator==(const FStamp&) const = default;
	};

	double StableSeconds = 1.0;

	void SetBaseline(const FStamp& Stamp);
	// 반환: 이번 호출에서 안정화가 끝났으면 true (기준이 관찰값으로 바뀐다). 파일이 없으면 기다리기만 한다.
	// bCanConsume = false면 안정화돼도 내보내지 않고 계속 기다린다 (파일을 아직 다른 프로세스가 쓰는 중)
	bool Update(double NowSeconds, const FStamp& Observed, bool bCanConsume = true);
	bool IsPending() const { return bPending; }

private:
	FStamp Baseline;
	FStamp LastObserved;
	double LastChangeTime = 0.0;
	bool   bPending       = false;
};

// 게임 모듈 타깃 백그라운드 빌드: 에디터 실행 파일이 놓인 빌드 폴더(CMakeCache.txt)의 CMake로 `cmake --build <폴더> --target <모듈>`.
// Ninja + cl은 VS 개발자 환경 변수가 필요하므로 Scripts/Build.ps1처럼 VsDevCmd.bat을 거친다(못 찾으면 cmake 직접).
// 출력은 작업 스레드가 줄 단위로 모으고 메인 스레드가 Poll로 가져간다. 프로세스는 잡 객체에 묶어 에디터가 끝나면 함께 끝난다
class FGameModuleBuild
{
public:
	FGameModuleBuild() = default;
	~FGameModuleBuild();
	FGameModuleBuild(const FGameModuleBuild&)            = delete;
	FGameModuleBuild& operator=(const FGameModuleBuild&) = delete;

	bool Start(const std::string& Target, std::string& OutError);
	bool IsRunning() const { return bRunning; }
	double GetElapsedSeconds() const;
	// 메인 스레드: 새 출력 줄. 끝났으면 OutFinished에 성공 여부 (한 번만)
	void Poll(std::vector<std::string>& OutLines, std::optional<bool>& OutFinished);
	void Cancel(); // 실행 중이면 프로세스 트리를 끝낸다

	// 실행 파일 폴더부터 위로 CMakeCache.txt를 찾는다 (최대 4단계)
	static std::filesystem::path FindBuildDirectory();

private:
	void Join();

	std::thread              Worker;
	std::mutex               Mutex;
	std::vector<std::string> PendingLines;
	std::optional<bool>      Finished;
	std::atomic<bool>        bRunning{ false };
	void*                    Job     = nullptr; // HANDLE
	void*                    Process = nullptr; // HANDLE
	double                   StartTime = 0.0;
};

// 에디터가 소유하는 핫 리로드 상태: 원본 경로, 복사본 폴더/번호, 원본 변경 감지, 빌드
class FGameModuleHotReload
{
public:
	// 원본 DLL(실행 파일 폴더의 <Name>.dll)과 복사본 폴더. 이전 실행이 남긴 복사본(<Name>_*.dll/.pdb)을 지운다(잠긴 것은 건너뜀)
	void Init(const std::string& ModuleName, const std::filesystem::path& SourceDll, const std::filesystem::path& ShadowDirectory);
	bool IsConfigured() const { return !ModuleName.empty(); }
	const std::string&           GetModuleName() const { return ModuleName; }
	const std::filesystem::path& GetSourceDll() const { return SourceDll; }

	// 원본 → 새 복사본 (+ PDB, PDB 경로 바꿈). 실패하면 빈 경로 + OutError
	std::filesystem::path MakeShadowCopy(std::string& OutError);

	// 원본 변경 감지 (0.5초마다 파일 시각 확인 — 매 프레임 불러도 된다). 반환: 안정화되어 다시 로드할 때 한 번 true
	bool PollSourceChange(double NowSeconds);
	void ResetSourceBaseline(); // 다시 로드 직후: 지금 원본 상태를 기준으로

	FGameModuleBuild& GetBuild() { return Build; }

private:
	FFileChangeDebouncer::FStamp ReadSourceStamp() const;
	bool                         IsSourceWritable() const; // 다른 프로세스(링커)가 쓰는 중이 아님

	std::string           ModuleName;
	std::filesystem::path SourceDll;
	std::filesystem::path ShadowDirectory;
	uint32                NextNumber   = 1;
	double                LastPollTime = -1.0;
	FFileChangeDebouncer  Debouncer;
	FGameModuleBuild      Build;
};

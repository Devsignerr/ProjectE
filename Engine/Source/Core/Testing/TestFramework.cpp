#include "Core/Testing/TestFramework.h"

#include <process.h>
#include <string>
#include <system_error>
#include <vector>

E_DEFINE_LOG_CATEGORY(LogTest, Log)

namespace
{
	struct FTestCase
	{
		const char*   Name;
		FTestFunction Function;
	};

	// 정적 초기화 순서 문제를 피하기 위해 함수 지역 정적 변수 사용
	std::vector<FTestCase>& GetTestCases()
	{
		static std::vector<FTestCase> TestCases;
		return TestCases;
	}

	int32 GCurrentTestFailures = 0;
} // namespace

void FTestRegistry::Register(const char* Name, FTestFunction Function)
{
	GetTestCases().push_back({ Name, Function });
}

int32 FTestRegistry::RunAll()
{
	const std::vector<FTestCase>& TestCases = GetTestCases();

	int32 PassedCount = 0;
	int32 FailedCount = 0;

	E_LOG(LogTest, Display, "테스트 {}개 실행", TestCases.size());

	for (const FTestCase& TestCase : TestCases)
	{
		GCurrentTestFailures = 0;
		TestCase.Function();

		if (GCurrentTestFailures == 0)
		{
			++PassedCount;
			E_LOG(LogTest, Log, "[통과] {}", TestCase.Name);
		}
		else
		{
			++FailedCount;
			E_LOG(LogTest, Error, "[실패] {} ({}건)", TestCase.Name, GCurrentTestFailures);
		}
	}

	// 이번 실행의 임시 폴더 정리 (열린 파일 등으로 실패해도 무시 — 다음 실행은 다른 폴더를 쓴다)
	std::error_code Error;
	std::filesystem::remove_all(GetTempDirectory(), Error);

	if (FailedCount == 0)
	{
		E_LOG(LogTest, Display, "결과: 전체 {}개 통과", PassedCount);
	}
	else
	{
		E_LOG(LogTest, Error, "결과: {}개 통과, {}개 실패", PassedCount, FailedCount);
	}
	return FailedCount;
}

std::filesystem::path FTestRegistry::GetTempDirectory()
{
	// 프로세스 ID로 나눠 여러 세션/worktree의 테스트가 동시에 돌아도 파일이 섞이지 않게 한다
	const std::filesystem::path Directory = std::filesystem::temp_directory_path() / L"ProjectE_Tests" / std::to_wstring(_getpid());
	std::error_code             Error;
	std::filesystem::create_directories(Directory, Error);
	return Directory;
}

void FTestRegistry::ReportFailure(const char* File, int32 Line, const std::string& Message)
{
	++GCurrentTestFailures;
	E_LOG(LogTest, Error, "  {}({}): {}", File, Line, Message);
}

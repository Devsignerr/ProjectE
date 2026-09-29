#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"

int main()
{
	FLog::Init();
	FPaths::Initialize(FCommandLine::Parse(L"")); // 인자 없이: 엔진 디렉터리 + 기본 예제 프로젝트
	const int32 FailedCount = FTestRegistry::RunAll();
	FLog::Shutdown();
	return FailedCount == 0 ? 0 : 1;
}

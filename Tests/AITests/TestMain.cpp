#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Testing/TestFramework.h"

int main()
{
	FLog::Init();
	FPaths::Initialize(FCommandLine::Parse(L""));
	const int32 FailedCount = FTestRegistry::RunAll();
	FLog::Shutdown();
	return FailedCount == 0 ? 0 : 1;
}

#include "Core/Testing/TestFramework.h"

int main()
{
	FLog::Init();
	const int32 FailedCount = FTestRegistry::RunAll();
	FLog::Shutdown();
	return FailedCount == 0 ? 0 : 1;
}

#include "Network/NetBindPolicy.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Network/NetTypes.h"

namespace
{
	bool GLoopbackOnly = false; // 메인 스레드에서 세션 시작 전에만 바뀐다
}

void NetBindPolicy::Configure(bool bDefaultLoopbackOnly)
{
	const FCommandLine& Args = FCommandLine::FromProcess();
	GLoopbackOnly            = Args.HasFlag(L"--net-public") ? false : (Args.HasFlag(L"--net-local") || bDefaultLoopbackOnly);
	E_LOG(LogNet, Log, "네트워크 대기 주소: {}", GLoopbackOnly ? "루프백 전용 (같은 PC)" : "모든 주소 (LAN)");
}

bool NetBindPolicy::IsLoopbackOnly()
{
	return GLoopbackOnly;
}

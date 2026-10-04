#include "Core/RenderThreadSync.h"

#include <atomic>

namespace
{
	std::atomic<RenderThreadSync::FWaitHandler> GWaitHandler{ nullptr };
}

void RenderThreadSync::SetWaitHandler(FWaitHandler Handler)
{
	GWaitHandler.store(Handler, std::memory_order_release);
}

void RenderThreadSync::WaitForRenderThread()
{
	if (const FWaitHandler Handler = GWaitHandler.load(std::memory_order_acquire); Handler != nullptr)
	{
		Handler();
	}
}

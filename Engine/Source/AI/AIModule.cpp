#include "AI/AIModule.h"

E_DEFINE_LOG_CATEGORY(LogAI, Log)

void RegisterAITypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	// 컴포넌트 등록은 Phase 18 단계 4에서 추가한다
}

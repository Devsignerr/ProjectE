#include "Core/Log.h"
#include "Scene/GameModule.h"

E_DEFINE_LOG_CATEGORY(LogFarmBie, Log)

// FarmBie 게임 모듈: 농장 격자·좀비·디펜스처럼 매 프레임 많이 도는 시스템을 C++로 둔다 (단계별로 채움 — Plans.md FarmBie)
class FFarmBieGameModule final : public IGameModule
{
public:
	void OnLoad() override
	{
		E_LOG(LogFarmBie, Display, "FarmBie 게임 모듈 로드");
	}
};

E_IMPLEMENT_GAME_MODULE(FFarmBieGameModule)

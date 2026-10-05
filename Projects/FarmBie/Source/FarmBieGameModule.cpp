#include "FarmBieComponents.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/GameModule.h"

E_DEFINE_LOG_CATEGORY(LogFarmBie, Log)

// FarmBie 게임 모듈: 농장 격자·좀비·디펜스처럼 매 프레임 많이 도는 시스템을 C++로 둔다 (단계별로 채움 — Plans.md FarmBie)
class FFarmBieGameModule final : public IGameModule
{
public:
	void OnLoad() override
	{
		FTypeRegistry& Registry = FTypeRegistry::Get();
		Registry.RegisterType<FFarmStructureComponent>("FarmStructureComponent", "농장 설치물 (FarmBie)")
			.Property(&FFarmStructureComponent::Kind, "Kind", "종류 (Buildables 행)")
			.Property(&FFarmStructureComponent::Hp, "Hp", "내구도").Range(0.0f, 100000.0f, 1.0f)
			.Property(&FFarmStructureComponent::MaxHp, "MaxHp", "최대 내구도").Range(1.0f, 100000.0f, 1.0f)
			.Property(&FFarmStructureComponent::TX, "TX", "격자 X")
			.Property(&FFarmStructureComponent::TY, "TY", "격자 Y")
			.Property(&FFarmStructureComponent::bBlocks, "Blocks", "길을 막음")
			.Property(&FFarmStructureComponent::bCrystal, "Crystal", "크리스탈")
			.Property(&FFarmStructureComponent::bDestroyed, "Destroyed", "부서짐")
			.AsComponent();
		E_LOG(LogFarmBie, Display, "FarmBie 게임 모듈 로드");
	}
};

E_IMPLEMENT_GAME_MODULE(FFarmBieGameModule)

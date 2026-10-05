#include "FarmBieComponents.h"
#include "FarmDefenseSystem.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/GameModule.h"

E_DEFINE_LOG_CATEGORY(LogFarmBie, Log)

// FarmBie 게임 모듈: 농장 격자·좀비·디펜스처럼 매 프레임 많이 도는 시스템을 C++로 둔다 (Plans.md FarmBie, Docs/Rules/FarmBie.md)
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
			.Property(&FFarmStructureComponent::Damage, "Damage", "덫·포탑 피해").Range(0.0f, 10000.0f, 1.0f)
			.Property(&FFarmStructureComponent::Radius, "Radius", "덫 범위 (cm)").Range(0.0f, 5000.0f, 1.0f)
			.Property(&FFarmStructureComponent::Cooldown, "Cooldown", "재사용 (초)").Range(0.0f, 60.0f, 0.05f)
			.Property(&FFarmStructureComponent::Range, "Range", "포탑 사거리 (cm)").Range(0.0f, 10000.0f, 1.0f)
			.AsComponent();
		Registry.RegisterType<FFarmZombieComponent>("FarmZombieComponent", "좀비 (FarmBie)")
			.Property(&FFarmZombieComponent::Kind, "Kind", "종류 (Zombies 행)")
			.Property(&FFarmZombieComponent::SpriteBase, "SpriteBase", "플립북 경로 앞부분")
			.Property(&FFarmZombieComponent::Hp, "Hp", "체력").Range(0.0f, 100000.0f, 1.0f)
			.Property(&FFarmZombieComponent::MaxHp, "MaxHp", "최대 체력").Range(1.0f, 100000.0f, 1.0f)
			.Property(&FFarmZombieComponent::Speed, "Speed", "속도 (cm/s)").Range(0.0f, 2000.0f, 1.0f)
			.Property(&FFarmZombieComponent::Damage, "Damage", "공격력").Range(0.0f, 10000.0f, 1.0f)
			.Property(&FFarmZombieComponent::AttackInterval, "AttackInterval", "공격 간격 (초)").Range(0.05f, 10.0f, 0.05f)
			.Property(&FFarmZombieComponent::StructureDamageMul, "StructureDamageMul", "설치물 피해 배율").Range(0.0f, 10.0f, 0.05f)
			.Property(&FFarmZombieComponent::CropEatTime, "CropEatTime", "작물 먹는 시간 (초)").Range(0.1f, 60.0f, 0.1f)
			.Property(&FFarmZombieComponent::BodyRadius, "BodyRadius", "몸 반지름 (cm)").Range(5.0f, 300.0f, 1.0f)
			.Property(&FFarmZombieComponent::bExplode, "Explode", "자폭")
			.Property(&FFarmZombieComponent::ExplodeRadius, "ExplodeRadius", "자폭 반경 (cm)").Range(0.0f, 2000.0f, 1.0f)
			.Property(&FFarmZombieComponent::ExplodeDamage, "ExplodeDamage", "자폭 피해").Range(0.0f, 10000.0f, 1.0f)
			.Property(&FFarmZombieComponent::RegenPerSec, "RegenPerSec", "초당 회복").Range(0.0f, 1000.0f, 0.1f)
			.Property(&FFarmZombieComponent::SlowOnHit, "SlowOnHit", "때리면 플레이어 둔화").Range(0.0f, 1.0f, 0.05f)
			.Property(&FFarmZombieComponent::bBoss, "Boss", "보스")
			.Property(&FFarmZombieComponent::AggroTime, "AggroTime", "맞았을 때 플레이어를 쫓는 시간 (초)").Range(0.0f, 60.0f, 0.1f)
			.Property(&FFarmZombieComponent::bAlerted, "Alerted", "크리스탈을 알아챔")
			.Property(&FFarmZombieComponent::bDead, "Dead", "죽음")
			.Property(&FFarmZombieComponent::SummonKind, "SummonKind", "소환 종류")
			.Property(&FFarmZombieComponent::SummonInterval, "SummonInterval", "소환 간격 (초)").Range(0.0f, 120.0f, 0.1f)
			.Property(&FFarmZombieComponent::SummonCount, "SummonCount", "소환 수")
			.Property(&FFarmZombieComponent::AuraRadius, "AuraRadius", "오라 반경").Range(0.0f, 5000.0f, 1.0f)
			.Property(&FFarmZombieComponent::AuraStructureDps, "AuraStructureDps", "오라 설치물 초당 피해").Range(0.0f, 1000.0f, 0.1f)
			.Property(&FFarmZombieComponent::AuraHeal, "AuraHeal", "오라 좀비 초당 회복").Range(0.0f, 1000.0f, 0.1f)
			.Property(&FFarmZombieComponent::bAuraSlow, "AuraSlow", "오라 플레이어 둔화")
			.AsComponent();
		Registry.RegisterType<FFarmDefenseComponent>("FarmDefenseComponent", "디펜스 상태 (FarmBie)")
			.Property(&FFarmDefenseComponent::bActive, "Active", "밤 디펜스 중")
			.Property(&FFarmDefenseComponent::OriginX, "OriginX", "격자 원점 X")
			.Property(&FFarmDefenseComponent::OriginY, "OriginY", "격자 원점 Y")
			.Property(&FFarmDefenseComponent::Tile, "Tile", "칸 크기")
			.Property(&FFarmDefenseComponent::Width, "Width", "가로 칸")
			.Property(&FFarmDefenseComponent::Height, "Height", "세로 칸")
			.Property(&FFarmDefenseComponent::StaticBlocked, "StaticBlocked", "막힌 칸 tx,ty;")
			.Property(&FFarmDefenseComponent::AttractTiles, "AttractTiles", "모여드는 칸 tx,ty;")
			.Property(&FFarmDefenseComponent::CropTiles, "CropTiles", "작물 칸 tx,ty;")
			.Property(&FFarmDefenseComponent::CropRevision, "CropRevision", "작물 칸 개정")
			.Property(&FFarmDefenseComponent::CrystalDetectRadius, "CrystalDetectRadius", "크리스탈 감지 반경").Range(0.0f, 5000.0f, 1.0f)
			.Property(&FFarmDefenseComponent::AlertRadius, "AlertRadius", "발견 호출 반경").Range(0.0f, 5000.0f, 1.0f)
			.Property(&FFarmDefenseComponent::BlockCost, "BlockCost", "설치물 칸 비용").Range(0.0f, 100.0f, 0.1f)
			.Property(&FFarmDefenseComponent::AttackSeq, "AttackSeq", "공격 명령 번호")
			.Property(&FFarmDefenseComponent::AttackKind, "AttackKind", "Melee | Shot")
			.Property(&FFarmDefenseComponent::AttackPos, "AttackPos", "공격 위치")
			.Property(&FFarmDefenseComponent::AttackDir, "AttackDir", "공격 방향")
			.Property(&FFarmDefenseComponent::AttackRange, "AttackRange", "공격 거리")
			.Property(&FFarmDefenseComponent::AttackArc, "AttackArc", "근접 부채꼴 (도)")
			.Property(&FFarmDefenseComponent::AttackDamage, "AttackDamage", "공격 피해")
			.Property(&FFarmDefenseComponent::AttackKnockback, "AttackKnockback", "밀쳐냄")
			.Property(&FFarmDefenseComponent::ShotCount, "ShotCount", "투사체 수")
			.Property(&FFarmDefenseComponent::ShotSpread, "ShotSpread", "퍼짐 (도)")
			.Property(&FFarmDefenseComponent::ShotSpeed, "ShotSpeed", "투사체 속도")
			.Property(&FFarmDefenseComponent::ShotSlice, "ShotSlice", "투사체 그림")
			.Property(&FFarmDefenseComponent::AttackHits, "AttackHits", "맞힌 수")
			.Property(&FFarmDefenseComponent::Kills, "Kills", "처치")
			.Property(&FFarmDefenseComponent::Alive, "Alive", "살아 있는 좀비")
			.Property(&FFarmDefenseComponent::PlayerDamage, "PlayerDamage", "플레이어가 받은 피해 (누적)")
			.Property(&FFarmDefenseComponent::PlayerSlow, "PlayerSlow", "플레이어 둔화 남은 시간")
			.Property(&FFarmDefenseComponent::bCrystalFound, "CrystalFound", "크리스탈 발견됨")
			.Property(&FFarmDefenseComponent::Alerts, "Alerts", "발견 횟수")
			.Property(&FFarmDefenseComponent::CropEvents, "CropEvents", "먹힌 작물 칸 tx,ty;")
			.Property(&FFarmDefenseComponent::Explosions, "Explosions", "폭발 수")
			.Property(&FFarmDefenseComponent::TurretShots, "TurretShots", "포탑 발사 수")
			.Property(&FFarmDefenseComponent::TrapHits, "TrapHits", "덫 명중 수")
			.Property(&FFarmDefenseComponent::FlowBuilds, "FlowBuilds", "흐름장 계산 수")
			.Property(&FFarmDefenseComponent::CrystalHp, "CrystalHp", "크리스탈 내구도")
			.Property(&FFarmDefenseComponent::BossAggro, "BossAggro", "보스 어그로 횟수")
			.Property(&FFarmDefenseComponent::SummonRequests, "SummonRequests", "보스 소환 요청 종류,x,y;")
			.Property(&FFarmDefenseComponent::AuraTicks, "AuraTicks", "보스 오라 적용 수")
			.AsComponent();
		E_LOG(LogFarmBie, Display, "FarmBie 게임 모듈 로드");
	}

	void OnBeginPlay(FScene& Scene) override
	{
		(void)Scene;
		Defense.Reset();
	}

	void OnEndPlay(FScene& Scene) override
	{
		(void)Scene;
		Defense.Reset();
	}

	void OnUpdate(FScene& Scene, float DeltaSeconds) override
	{
		Defense.Update(Scene, DeltaSeconds);
	}

private:
	FFarmDefenseSystem Defense;
};

E_IMPLEMENT_GAME_MODULE(FFarmBieGameModule)

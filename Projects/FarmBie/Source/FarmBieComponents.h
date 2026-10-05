#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <string>

// FarmBie 게임 컴포넌트 (FarmBieGameModule.cpp에서 리플렉션 등록 → 인스펙터/직렬화/Lua에 자동 노출).
// 규약: Lua가 엔티티를 만들고 등록 필드를 채운다(수치는 데이터 표) → C++ 디펜스 시스템(FarmDefenseSystem)이 매 프레임 읽고 움직이고 피해를 준다.
//        등록하지 않은 필드는 C++ 런타임 상태(직렬화 제외).

// 격자 위 설치물 (벽·문·덫·포탑·크리스탈). 칸 번호는 Data/FarmBie/FarmMap.edata 격자 기준
struct FFarmStructureComponent
{
	std::string Kind;              // Buildables.etable 행 이름 (Crystal = 크리스탈)
	float       Hp         = 100.0f;
	float       MaxHp      = 100.0f;
	int32       TX         = 0;
	int32       TY         = 0;
	bool        bBlocks    = true;  // 좀비 길을 막음 (부숴야 지나감) — 덫·지뢰는 false
	bool        bCrystal   = false;
	bool        bDestroyed = false; // Hp가 0이 되면 디펜스 시스템이 켠다 → Lua가 정리(엔티티 삭제·격자 비움)
	// 덫·포탑 (Buildables.etable — Spike: 반경 안 좀비를 Cooldown마다 / Mine: 밟으면 한 번 폭발 / Turret: 사거리 안 가장 가까운 좀비에게 화살)
	float       Damage   = 0.0f;
	float       Radius   = 0.0f;
	float       Cooldown = 0.0f;
	float       Range    = 0.0f;

	// 런타임 (등록하지 않음)
	float Timer = 0.0f;
};

// 좀비 (보스 포함). Zombies.etable 값을 Lua가 채운다. 그림 = 자식 "Body"(빌보드 스프라이트 + 플립북)
//   플립북 경로 = SpriteBase + "_<Walk|Attack|Die><Side|Down>.eflipbook" (Side는 오른쪽을 봄 — 왼쪽은 좌우 반전)
struct FFarmZombieComponent
{
	std::string Kind;
	std::string SpriteBase;                 // 예: Sprites/FarmBie/Zombie_Walker
	float       Hp                 = 30.0f;
	float       MaxHp              = 30.0f;
	float       Speed              = 120.0f; // cm/s
	float       Damage             = 6.0f;   // 한 번 칠 때 (플레이어·설치물·크리스탈)
	float       AttackInterval     = 1.0f;
	float       StructureDamageMul = 1.0f;
	float       CropEatTime        = 2.0f;   // 작물 한 칸 먹는 시간 (초)
	float       BodyRadius         = 30.0f;
	bool        bExplode           = false;  // 자폭: 공격 대신 터짐
	float       ExplodeRadius      = 0.0f;
	float       ExplodeDamage      = 0.0f;
	float       RegenPerSec        = 0.0f;
	float       SlowOnHit          = 0.0f;   // 플레이어를 때리면 이 비율만큼 느리게 (초 단위 지속은 디펜스 상태)
	bool        bBoss              = false;  // 보스: 플레이어를 먼저 노리지 않음 (맞으면 잠깐 어그로 — AggroTime)
	float       AggroTime          = 0.0f;   // 보스가 맞았을 때 플레이어를 쫓는 시간 (초)
	bool        bAlerted           = false;  // 크리스탈을 알아챔 → 크리스탈로 돌진
	bool        bDead              = false;

	// 런타임 (등록하지 않음)
	FEntity  Body;
	float    AttackTimer  = 0.0f;
	float    CropTimer    = 0.0f;
	float    DeathTimer   = 0.0f;
	float    FlashTimer   = 0.0f;
	float    AggroTimer   = 0.0f;
	float    ExplodeTimer = -1.0f;       // 자폭 준비 (깜빡임) — 0 이하가 되면 터짐
	FVector3 Knockback    = FVector3::ZeroVector;
	FVector3 Facing       = FVector3(0.0f, 1.0f, 0.0f);
	int32    AnimState    = -1;          // 0 걷기 1 공격 2 죽음
	int32    AnimDir      = -1;          // 0 옆 1 아래
	bool     bAnimFlip    = false;
	bool     bInit        = false;
};

// 디펜스 상태·명령 창구 (FarmGame 엔티티에 하나). Lua ↔ C++ 약속:
//   Lua 입력: Active(밤), 격자 정보, 정적으로 막힌 칸·끌어당기는 칸, 작물 칸(Revision이 바뀔 때만 다시 읽음), 크리스탈 감지 반경, 플레이어 공격 명령(AttackSeq 증가)
//   C++ 출력: 처치·생존 수, 플레이어가 받은 피해(PlayerDamage 누적 — Lua가 읽고 0으로), 먹힌 작물 칸(CropEvents "tx,ty;" 누적 — Lua가 비움), 크리스탈 발견 등
struct FFarmDefenseComponent
{
	bool        bActive        = false;
	float       OriginX        = 0.0f;
	float       OriginY        = 0.0f;
	float       Tile           = 100.0f;
	int32       Width          = 0;
	int32       Height         = 0;
	std::string StaticBlocked;            // "tx,ty;..." 좀비가 못 지나가는 칸 (울타리·건물)
	std::string AttractTiles;             // "tx,ty;..." 좀비가 모여드는 칸 (집 앞마당)
	std::string CropTiles;                // "tx,ty;..." 살아 있는 작물 칸
	int32       CropRevision   = 0;       // CropTiles를 바꿀 때마다 올린다
	float       CrystalDetectRadius = 650.0f;
	float       AlertRadius    = 900.0f;  // 발견한 좀비가 부르는 반경
	float       BlockCost      = 6.0f;    // 흐름장: 설치물 칸 비용 = 1 + BlockCost × 내구도/100 (약한 벽으로 몰린다)
	// 플레이어 공격 명령: AttackSeq를 올리면 다음 갱신에 한 번 처리
	int32       AttackSeq      = 0;
	std::string AttackKind;               // Melee | Shot
	FVector3    AttackPos      = FVector3::ZeroVector;
	FVector3    AttackDir      = FVector3(0.0f, 1.0f, 0.0f);
	float       AttackRange    = 150.0f;  // 근접 거리 / 투사체 사거리
	float       AttackArc      = 120.0f;  // 근접 부채꼴 (도)
	float       AttackDamage   = 10.0f;
	float       AttackKnockback = 400.0f;
	int32       ShotCount      = 1;
	float       ShotSpread     = 0.0f;    // 도
	float       ShotSpeed      = 1600.0f;
	std::string ShotSlice;                // Fx.esprite 슬라이스
	// 출력
	int32       AttackHits     = 0;       // 마지막 공격 명령이 맞힌 수 (투사체는 맞을 때마다 더함)
	int32       Kills          = 0;
	int32       Alive          = 0;
	float       PlayerDamage   = 0.0f;
	float       PlayerSlow     = 0.0f;    // 남은 둔화 시간 (초)
	bool        bCrystalFound  = false;
	int32       Alerts         = 0;
	std::string CropEvents;
	int32       Explosions     = 0;
	int32       TurretShots    = 0;
	int32       TrapHits       = 0;
	int32       FlowBuilds     = 0;
	float       CrystalHp      = 0.0f;
	int32       BossAggro      = 0;       // 보스가 플레이어를 쫓기 시작한 횟수

	// 런타임 (등록하지 않음)
	int32 LastAttackSeq = 0;
	int32 LastCropRevision = -1;
};

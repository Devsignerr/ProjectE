#pragma once

#include "FarmBieComponents.h"

#include <string>
#include <vector>

class FScene;

// FarmBie 밤 디펜스 시스템 (게임 모듈 OnUpdate에서 매 프레임). 좀비가 많아도 C++에서 돈다 (CLAUDE.md: 매 프레임 대량 순회는 C++).
//   흐름장: 농장 격자(FFarmDefenseComponent의 격자 정보) 위 다익스트라 두 장 —
//     Farm    = 작물 칸·끌어당기는 칸(집 앞)까지 (크리스탈을 아직 모르는 좀비)
//     Crystal = 크리스탈 칸까지 (알아챈 좀비)
//     칸 비용: 빈 칸 1, 막는 설치물 칸 1 + BlockCost × 내구도/100 (약한 벽으로 몰린다), 정적으로 막힌 칸(울타리·건물)은 못 감. 대각선은 양옆이 막히지 않을 때만
//   좀비: 이웃 8칸 중 흐름값이 가장 낮은 칸으로 걷고, 그 칸에 막는 설치물·크리스탈이 있으면 멈춰서 친다. 작물 칸에 서면 CropEatTime 뒤 먹음(CropEvents).
//     크리스탈 감지 반경 안에 들어오면 알아채고(bAlerted) AlertRadius 안의 다른 좀비도 부른다(한 번 — 연쇄는 부른 좀비가 다시 발견할 때만).
//     보스가 아닌 좀비는 플레이어가 가까우면 플레이어를 친다. 보스는 맞았을 때 AggroTime 동안만 플레이어를 쫓는다.
//   덫·포탑: 가시덫(반경 안 Cooldown마다, 덫도 닳음) · 지뢰(밟으면 폭발, 한 번) · 포탑(사거리 안 가장 가까운 좀비에게 화살 투사체)
//   투사체·폭발 효과는 C++가 엔티티를 만들고 지운다 (스프라이트 Sprites/FarmBie/Fx.esprite)
class FFarmDefenseSystem
{
public:
	void Update(FScene& Scene, float DeltaSeconds);
	void Reset();

private:
	struct FProjectile
	{
		FEntity  Entity;
		FVector3 Pos;
		FVector3 Vel;
		float    Damage   = 0.0f;
		float    Knock    = 0.0f;
		float    Life     = 0.0f;
		bool     bPlayer  = false;
	};
	struct FEffect
	{
		FEntity Entity;
		float   Life = 0.0f;
	};

	void SyncGrid(FFarmDefenseComponent& Defense);
	void BuildFields(FScene& Scene, FFarmDefenseComponent& Defense);
	void Dijkstra(const std::vector<int32>& Targets, std::vector<float>& OutField) const;
	void HandlePlayerAttack(FScene& Scene, FFarmDefenseComponent& Defense);
	void UpdateZombies(FScene& Scene, FFarmDefenseComponent& Defense, float Dt);
	void UpdateTraps(FScene& Scene, FFarmDefenseComponent& Defense, float Dt);
	void UpdateProjectiles(FScene& Scene, FFarmDefenseComponent& Defense, float Dt);
	void UpdateEffects(FScene& Scene, float Dt);
	void DamageZombie(FScene& Scene, FEntity Entity, FFarmZombieComponent& Zombie, float Amount, const FVector3& Push, FFarmDefenseComponent& Defense,
	                  bool bFromPlayer);
	void Explode(FScene& Scene, const FVector3& Pos, float Radius, float Damage, bool bHurtZombies, bool bHurtStructures, FFarmDefenseComponent& Defense);
	void DamageStructure(FFarmStructureComponent& Structure, float Amount);
	void SpawnProjectile(FScene& Scene, const FVector3& From, const FVector3& Dir, float Speed, float Range, float Damage, float Knock, bool bPlayer,
	                     const std::string& Slice);
	void SpawnEffect(FScene& Scene, const FVector3& Pos, const char* Flipbook, const char* Slice, float Life, float Scale);
	void UpdateZombieLook(FScene& Scene, FFarmZombieComponent& Zombie, int32 State, const FVector3& Move);
	FEntity FindPlayer(FScene& Scene);

	int32 CellOf(const FVector3& Pos) const;
	FVector3 CellCenter(int32 Cell) const;
	bool InGrid(int32 X, int32 Y) const { return X >= 0 && Y >= 0 && X < Width && Y < Height; }

	// 격자
	int32 Width = 0;
	int32 Height = 0;
	float OriginX = 0.0f;
	float OriginY = 0.0f;
	float Tile = 100.0f;
	std::string StaticSource;
	std::string AttractSource;
	std::vector<uint8> StaticBlocked;
	std::vector<int32> AttractCells;
	std::vector<uint8> CropCells;
	// 설치물 (프레임마다 다시 모음)
	std::vector<FEntity> CellStructure;   // 칸 → 막는 설치물 (크리스탈 포함)
	std::vector<float>   CellCost;
	int32                CrystalCell = -1;
	FEntity              CrystalEntity;
	uint64               StructureSignature = 0;
	float                RebuildTimer = 0.0f;
	bool                 bFieldsDirty = true;
	std::vector<float>   FieldFarm;
	std::vector<float>   FieldCrystal;

	FEntity                  Player;
	std::vector<FProjectile> Projectiles;
	std::vector<FEffect>     Effects;
	std::vector<FEntity>     PendingDestroy;
	uint32                   RandomState = 12345u;
};

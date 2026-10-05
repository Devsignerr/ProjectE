#include "FarmDefenseSystem.h"

#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace
{
	constexpr float InfCost      = 1.0e9f;
	constexpr float Sqrt2        = 1.41421356f;
	constexpr float PlayerReach  = 120.0f;  // 좀비가 플레이어를 치는 거리 (몸 반지름 더함)
	constexpr float BossChaseNear = 160.0f;
	constexpr float DeathTime    = 0.7f;
	constexpr float FlashTime    = 0.12f;
	constexpr float CellsPerSecRebuild = 0.3f; // 흐름장 다시 계산 최소 간격 (초)
	const char*     FxSprite     = "Sprites/FarmBie/Fx.esprite";

	FVector3 Flat(const FVector3& V) { return FVector3(V.X, V.Y, 0.0f); }

	// "tx,ty;tx,ty;..." → 칸 번호 목록
	void ParseCells(const std::string& Text, int32 Width, int32 Height, std::vector<int32>& Out)
	{
		Out.clear();
		size_t Pos = 0;
		while (Pos < Text.size())
		{
			const size_t End   = Text.find(';', Pos);
			const std::string Item = Text.substr(Pos, End == std::string::npos ? std::string::npos : End - Pos);
			const size_t Comma = Item.find(',');
			if (Comma != std::string::npos)
			{
				const int32 X = std::atoi(Item.substr(0, Comma).c_str());
				const int32 Y = std::atoi(Item.substr(Comma + 1).c_str());
				if (X >= 0 && Y >= 0 && X < Width && Y < Height)
				{
					Out.push_back(Y * Width + X);
				}
			}
			if (End == std::string::npos)
			{
				break;
			}
			Pos = End + 1;
		}
	}

	FEntity FindChildByName(FScene& Scene, FEntity Parent, std::string_view Name)
	{
		for (const FEntity Child : Scene.GetChildren(Parent))
		{
			const FNameComponent* NameComp = Scene.GetRegistry().TryGet<FNameComponent>(Child);
			if (NameComp != nullptr && NameComp->Name == Name)
			{
				return Child;
			}
		}
		return NullEntity;
	}
}

void FFarmDefenseSystem::Reset()
{
	Projectiles.clear();
	Effects.clear();
	PendingDestroy.clear();
	Player             = NullEntity;
	StructureSignature = 0;
	bFieldsDirty       = true;
	Width = Height = 0;
	StaticSource.clear();
	AttractSource.clear();
}

int32 FFarmDefenseSystem::CellOf(const FVector3& Pos) const
{
	const int32 X = static_cast<int32>(std::floor((Pos.X - OriginX) / Tile));
	const int32 Y = static_cast<int32>(std::floor((Pos.Y - OriginY) / Tile));
	return InGrid(X, Y) ? Y * Width + X : -1;
}

FVector3 FFarmDefenseSystem::CellCenter(int32 Cell) const
{
	const int32 X = Cell % Width;
	const int32 Y = Cell / Width;
	return FVector3(OriginX + (static_cast<float>(X) + 0.5f) * Tile, OriginY + (static_cast<float>(Y) + 0.5f) * Tile, 0.0f);
}

FEntity FFarmDefenseSystem::FindPlayer(FScene& Scene)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (Registry.IsValid(Player))
	{
		return Player;
	}
	Player = NullEntity;
	Registry.View<FNameComponent>().Each([this](FEntity Entity, FNameComponent& Name) {
		if (Name.Name == "Player")
		{
			Player = Entity;
		}
	});
	return Player;
}

void FFarmDefenseSystem::SyncGrid(FFarmDefenseComponent& Defense)
{
	const bool bResize = Defense.Width != Width || Defense.Height != Height || Defense.Tile != Tile || Defense.OriginX != OriginX || Defense.OriginY != OriginY;
	if (bResize)
	{
		Width   = Defense.Width;
		Height  = Defense.Height;
		Tile    = Defense.Tile > 1.0f ? Defense.Tile : 100.0f;
		OriginX = Defense.OriginX;
		OriginY = Defense.OriginY;
		const size_t Count = static_cast<size_t>(std::max(0, Width * Height));
		StaticBlocked.assign(Count, 0);
		CropCells.assign(Count, 0);
		StaticSource.clear();
		AttractSource.clear();
		Defense.LastCropRevision = -1;
		bFieldsDirty = true;
	}
	if (Width <= 0 || Height <= 0)
	{
		return;
	}
	std::vector<int32> Cells;
	if (Defense.StaticBlocked != StaticSource)
	{
		StaticSource = Defense.StaticBlocked;
		std::fill(StaticBlocked.begin(), StaticBlocked.end(), uint8(0));
		ParseCells(StaticSource, Width, Height, Cells);
		for (const int32 Cell : Cells)
		{
			StaticBlocked[static_cast<size_t>(Cell)] = 1;
		}
		bFieldsDirty = true;
	}
	if (Defense.AttractTiles != AttractSource)
	{
		AttractSource = Defense.AttractTiles;
		ParseCells(AttractSource, Width, Height, AttractCells);
		bFieldsDirty = true;
	}
	if (Defense.CropRevision != Defense.LastCropRevision)
	{
		Defense.LastCropRevision = Defense.CropRevision;
		std::fill(CropCells.begin(), CropCells.end(), uint8(0));
		ParseCells(Defense.CropTiles, Width, Height, Cells);
		for (const int32 Cell : Cells)
		{
			CropCells[static_cast<size_t>(Cell)] = 1;
		}
		bFieldsDirty = true;
	}
}

void FFarmDefenseSystem::Dijkstra(const std::vector<int32>& Targets, std::vector<float>& OutField) const
{
	const size_t Count = static_cast<size_t>(Width * Height);
	OutField.assign(Count, InfCost);
	using FItem = std::pair<float, int32>;
	std::priority_queue<FItem, std::vector<FItem>, std::greater<FItem>> Open;
	for (const int32 Cell : Targets)
	{
		if (Cell >= 0 && static_cast<size_t>(Cell) < Count && !StaticBlocked[static_cast<size_t>(Cell)])
		{
			OutField[static_cast<size_t>(Cell)] = 0.0f;
			Open.push({ 0.0f, Cell });
		}
	}
	static constexpr int32 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static constexpr int32 DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	while (!Open.empty())
	{
		const auto [Dist, Cell] = Open.top();
		Open.pop();
		if (Dist > OutField[static_cast<size_t>(Cell)])
		{
			continue;
		}
		const int32 X = Cell % Width;
		const int32 Y = Cell / Width;
		for (int32 K = 0; K < 8; ++K)
		{
			const int32 NX = X + DX[K];
			const int32 NY = Y + DY[K];
			if (!InGrid(NX, NY))
			{
				continue;
			}
			const int32 Next = NY * Width + NX;
			if (StaticBlocked[static_cast<size_t>(Next)])
			{
				continue;
			}
			const bool bDiagonal = K >= 4;
			if (bDiagonal && (StaticBlocked[static_cast<size_t>(Y * Width + NX)] || StaticBlocked[static_cast<size_t>(NY * Width + X)] ||
			                  CellStructure[static_cast<size_t>(Y * Width + NX)].IsValid() || CellStructure[static_cast<size_t>(NY * Width + X)].IsValid()))
			{
				continue; // 모서리 끼어 지나가기 금지
			}
			// 들어가는 칸(이웃 → 현재 방향으로 걷는 좀비 기준: 현재 칸 비용)
			const float Step = (bDiagonal ? Sqrt2 : 1.0f) * CellCost[static_cast<size_t>(Cell)];
			const float NewDist = Dist + Step;
			if (NewDist < OutField[static_cast<size_t>(Next)])
			{
				OutField[static_cast<size_t>(Next)] = NewDist;
				Open.push({ NewDist, Next });
			}
		}
	}
}

void FFarmDefenseSystem::BuildFields(FScene& Scene, FFarmDefenseComponent& Defense)
{
	const size_t Count = static_cast<size_t>(Width * Height);
	CellStructure.assign(Count, NullEntity);
	CellCost.assign(Count, 1.0f);
	CrystalCell   = -1;
	CrystalEntity = NullEntity;
	uint64 Signature = 1469598103934665603ull;
	Scene.GetRegistry().View<FFarmStructureComponent>().Each([&](FEntity Entity, FFarmStructureComponent& S) {
		if (S.bDestroyed || !InGrid(S.TX, S.TY))
		{
			return;
		}
		const int32 Cell = S.TY * Width + S.TX;
		if (S.bCrystal)
		{
			CrystalEntity = Entity;
			if (S.bBlocks)
			{
				CrystalCell = Cell; // 들고 있는 동안(Blocks 끔)은 목표 없음
			}
		}
		if (S.bBlocks)
		{
			CellStructure[static_cast<size_t>(Cell)] = Entity;
			CellCost[static_cast<size_t>(Cell)]      = 1.0f + Defense.BlockCost * std::max(S.Hp, 1.0f) / 100.0f;
		}
		// 흐름장에 영향을 주는 것만 서명에 (칸·막음·내구도 25 단위)
		const uint64 Key = (static_cast<uint64>(Cell) << 20) ^ (static_cast<uint64>(S.bBlocks) << 19) ^ static_cast<uint64>(S.Hp / 25.0f);
		Signature = (Signature ^ Key) * 1099511628211ull;
	});
	const bool bChanged = Signature != StructureSignature;
	if (!bFieldsDirty && (!bChanged || RebuildTimer > 0.0f))
	{
		return;
	}
	StructureSignature = Signature;
	bFieldsDirty       = false;
	RebuildTimer       = CellsPerSecRebuild;
	std::vector<int32> Targets = AttractCells;
	for (size_t Cell = 0; Cell < Count; ++Cell)
	{
		if (CropCells[Cell])
		{
			Targets.push_back(static_cast<int32>(Cell));
		}
	}
	Dijkstra(Targets, FieldFarm);
	std::vector<int32> CrystalTargets;
	if (CrystalCell >= 0)
	{
		CrystalTargets.push_back(CrystalCell);
	}
	Dijkstra(CrystalTargets, FieldCrystal);
	++Defense.FlowBuilds;
}

void FFarmDefenseSystem::Update(FScene& Scene, float Dt)
{
	FRegistry&             Registry = Scene.GetRegistry();
	FFarmDefenseComponent* Defense  = nullptr;
	Registry.View<FFarmDefenseComponent>().Each([&](FEntity, FFarmDefenseComponent& D) { Defense = &D; });
	if (Defense == nullptr)
	{
		return;
	}
	SyncGrid(*Defense);
	if (Width <= 0 || Height <= 0)
	{
		return;
	}
	RebuildTimer -= Dt;
	BuildFields(Scene, *Defense);
	FindPlayer(Scene);
	if (Defense->PlayerSlow > 0.0f)
	{
		Defense->PlayerSlow = std::max(0.0f, Defense->PlayerSlow - Dt);
	}

	HandlePlayerAttack(Scene, *Defense);
	UpdateZombies(Scene, *Defense, Dt);
	UpdateTraps(Scene, *Defense, Dt);
	UpdateProjectiles(Scene, *Defense, Dt);
	UpdateEffects(Scene, Dt);

	if (CrystalEntity.IsValid() && Registry.IsValid(CrystalEntity))
	{
		Defense->CrystalHp = Registry.Get<FFarmStructureComponent>(CrystalEntity).Hp;
	}
	for (const FEntity Entity : PendingDestroy)
	{
		Scene.DestroyEntity(Entity);
	}
	PendingDestroy.clear();
}

// ---- 플레이어 공격 (Lua가 AttackSeq를 올림)
void FFarmDefenseSystem::HandlePlayerAttack(FScene& Scene, FFarmDefenseComponent& Defense)
{
	if (Defense.AttackSeq == Defense.LastAttackSeq)
	{
		return;
	}
	Defense.LastAttackSeq = Defense.AttackSeq;
	Defense.AttackHits    = 0;
	const FVector3 Dir = Flat(Defense.AttackDir).IsNearlyZero() ? FVector3(0.0f, 1.0f, 0.0f) : Flat(Defense.AttackDir).GetNormalized();
	if (Defense.AttackKind == "Shot")
	{
		const int32 Count = std::max(1, Defense.ShotCount);
		for (int32 I = 0; I < Count; ++I)
		{
			const float T     = Count == 1 ? 0.0f : (static_cast<float>(I) / static_cast<float>(Count - 1) - 0.5f);
			const float Angle = FMath::DegreesToRadians(Defense.ShotSpread * T);
			const FVector3 D(Dir.X * std::cos(Angle) - Dir.Y * std::sin(Angle), Dir.X * std::sin(Angle) + Dir.Y * std::cos(Angle), 0.0f);
			SpawnProjectile(Scene, Defense.AttackPos + FVector3(0.0f, 0.0f, 70.0f), D, Defense.ShotSpeed, Defense.AttackRange, Defense.AttackDamage,
			                Defense.AttackKnockback, true, Defense.ShotSlice.empty() ? std::string("Arrow") : Defense.ShotSlice);
		}
		return;
	}
	// 근접: 부채꼴 안 좀비 모두
	const float CosHalf = std::cos(FMath::DegreesToRadians(Defense.AttackArc * 0.5f));
	Scene.GetRegistry().View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity Entity, FTransformComponent& T, FFarmZombieComponent& Z) {
		if (Z.bDead)
		{
			return;
		}
		const FVector3 To   = Flat(T.Position - Defense.AttackPos);
		const float    Dist = To.Length();
		if (Dist > Defense.AttackRange + Z.BodyRadius)
		{
			return;
		}
		if (Dist > 1.0f && FVector3::Dot(To / Dist, Dir) < CosHalf)
		{
			return;
		}
		DamageZombie(Scene, Entity, Z, Defense.AttackDamage, (Dist > 1.0f ? To / Dist : Dir) * Defense.AttackKnockback, Defense, true);
		++Defense.AttackHits;
	});
}

void FFarmDefenseSystem::DamageZombie(FScene& Scene, FEntity Entity, FFarmZombieComponent& Zombie, float Amount, const FVector3& Push,
                                      FFarmDefenseComponent& Defense, bool bFromPlayer)
{
	(void)Scene;
	(void)Entity;
	if (Zombie.bDead)
	{
		return;
	}
	Zombie.Hp -= Amount;
	Zombie.FlashTimer = FlashTime;
	Zombie.Knockback  = Zombie.Knockback + (Zombie.bBoss ? Push * 0.25f : Push);
	if (bFromPlayer && Zombie.bBoss && Zombie.AggroTime > 0.0f)
	{
		if (Zombie.AggroTimer <= 0.0f)
		{
			++Defense.BossAggro;
		}
		Zombie.AggroTimer = Zombie.AggroTime; // 잠깐만 어그로 (강해서 오래 끌 수 없다)
	}
	if (Zombie.Hp <= 0.0f)
	{
		Zombie.Hp         = 0.0f;
		Zombie.bDead      = true;
		Zombie.DeathTimer = DeathTime;
		++Defense.Kills;
	}
}

void FFarmDefenseSystem::DamageStructure(FFarmStructureComponent& Structure, float Amount)
{
	if (Structure.bDestroyed)
	{
		return;
	}
	Structure.Hp -= Amount;
	if (Structure.Hp <= 0.0f)
	{
		Structure.Hp         = 0.0f;
		Structure.bDestroyed = true;
		bFieldsDirty         = true;
	}
}

void FFarmDefenseSystem::Explode(FScene& Scene, const FVector3& Pos, float Radius, float Damage, bool bHurtZombies, bool bHurtStructures,
                                 FFarmDefenseComponent& Defense)
{
	FRegistry& Registry = Scene.GetRegistry();
	++Defense.Explosions;
	SpawnEffect(Scene, Pos + FVector3(0.0f, 0.0f, 10.0f), "Sprites/FarmBie/Fx_Boom.eflipbook", "Boom0", 0.42f, std::max(1.0f, Radius / 90.0f));
	if (bHurtZombies)
	{
		Registry.View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity Entity, FTransformComponent& T, FFarmZombieComponent& Z) {
			const FVector3 To = Flat(T.Position - Pos);
			const float    D  = To.Length();
			if (!Z.bDead && D < Radius + Z.BodyRadius)
			{
				DamageZombie(Scene, Entity, Z, Damage * (1.0f - 0.5f * D / (Radius + Z.BodyRadius)), (D > 1.0f ? To / D : FVector3(0, 1, 0)) * 700.0f,
				             Defense, false);
			}
		});
	}
	if (bHurtStructures)
	{
		Registry.View<FFarmStructureComponent>().Each([&](FEntity, FFarmStructureComponent& S) {
			if (!InGrid(S.TX, S.TY))
			{
				return;
			}
			const FVector3 C = CellCenter(S.TY * Width + S.TX);
			if (Flat(C - Pos).Length() < Radius + Tile * 0.4f)
			{
				DamageStructure(S, Damage);
			}
		});
		const FEntity P = Player;
		if (Registry.IsValid(P) && Flat(Registry.Get<FTransformComponent>(P).Position - Pos).Length() < Radius + 40.0f)
		{
			Defense.PlayerDamage += Damage * 0.5f;
		}
	}
}

// ---- 좀비
void FFarmDefenseSystem::UpdateZombieLook(FScene& Scene, FFarmZombieComponent& Zombie, int32 State, const FVector3& Move)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Zombie.Body))
	{
		return;
	}
	if (!Flat(Move).IsNearlyZero())
	{
		Zombie.Facing = Flat(Move).GetNormalized();
	}
	const int32 Dir   = std::abs(Zombie.Facing.X) >= std::abs(Zombie.Facing.Y) * 0.8f ? 0 : 1;
	const bool  bFlip = Dir == 0 && Zombie.Facing.X < 0.0f;
	FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Zombie.Body);
	if (Sprite != nullptr)
	{
		Sprite->bFlipX = bFlip;
		// 맞음 = 잠깐 붉게, 자폭 준비 = 붉게 깜빡. FlashColor 대신 Color 곱하기를 쓴다 — C++가 FlashColor를 매 프레임 쓰면 번쩍임이 남고
		// 같은 엔티티의 그림자 스프라이트까지 바래 보였다 (엔진 조사 항목, Plans.md)
		float Red = Zombie.FlashTimer > 0.0f ? 1.0f : 0.0f;
		if (Zombie.ExplodeTimer > 0.0f)
		{
			Red = 0.5f + 0.5f * std::sin(Zombie.ExplodeTimer * 40.0f);
		}
		Sprite->Color = FVector4(1.0f, 1.0f - 0.55f * Red, 1.0f - 0.6f * Red, 1.0f);
	}
	if (State == Zombie.AnimState && Dir == Zombie.AnimDir)
	{
		return;
	}
	Zombie.AnimState = State;
	Zombie.AnimDir   = Dir;
	static const char* States[3] = { "Walk", "Attack", "Die" };
	FFlipbookComponent* Book = Registry.TryGet<FFlipbookComponent>(Zombie.Body);
	if (Book != nullptr)
	{
		Book->Flipbook = Zombie.SpriteBase + "_" + States[State] + (Dir == 0 ? "Side" : "Down") + ".eflipbook";
		Book->bPlaying = true;
	}
}

void FFarmDefenseSystem::UpdateZombies(FScene& Scene, FFarmDefenseComponent& Defense, float Dt)
{
	FRegistry& Registry = Scene.GetRegistry();
	const bool bHasPlayer = Registry.IsValid(Player);
	const FVector3 PlayerPos = bHasPlayer ? Registry.Get<FTransformComponent>(Player).Position : FVector3::ZeroVector;
	const FVector3 CrystalPos = CrystalCell >= 0 ? CellCenter(CrystalCell) : FVector3::ZeroVector;

	// 위치 목록 (밀어내기·발견 알림용)
	struct FInfo
	{
		FEntity                Entity;
		FTransformComponent*   T;
		FFarmZombieComponent*  Z;
	};
	std::vector<FInfo> List;
	Registry.View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity Entity, FTransformComponent& T, FFarmZombieComponent& Z) {
		List.push_back({ Entity, &T, &Z });
	});
	int32 Alive = 0;
	for (FInfo& Info : List)
	{
		FFarmZombieComponent& Z = *Info.Z;
		FTransformComponent&  T = *Info.T;
		if (!Z.bInit)
		{
			Z.bInit = true;
			Z.Body  = FindChildByName(Scene, Info.Entity, "Body");
		}
		Z.FlashTimer = std::max(0.0f, Z.FlashTimer - Dt);
		if (Z.bDead)
		{
			Z.DeathTimer -= Dt;
			UpdateZombieLook(Scene, Z, 2, FVector3::ZeroVector);
			if (Z.DeathTimer <= 0.0f)
			{
				PendingDestroy.push_back(Info.Entity);
				Z.DeathTimer = 1.0e9f; // 한 번만
			}
			continue;
		}
		++Alive;
		if (Z.RegenPerSec > 0.0f)
		{
			Z.Hp = std::min(Z.MaxHp, Z.Hp + Z.RegenPerSec * Dt);
		}
		Z.AggroTimer  = std::max(0.0f, Z.AggroTimer - Dt);
		Z.AttackTimer = std::max(0.0f, Z.AttackTimer - Dt);
		// 넉백 (감속)
		if (!Z.Knockback.IsNearlyZero())
		{
			T.Position = T.Position + Z.Knockback * Dt;
			const float Len = Z.Knockback.Length();
			const float NewLen = std::max(0.0f, Len - 2400.0f * Dt);
			Z.Knockback = Len > 0.0f ? Z.Knockback * (NewLen / Len) : FVector3::ZeroVector;
		}
		// 자폭 준비 중
		if (Z.ExplodeTimer > 0.0f)
		{
			Z.ExplodeTimer -= Dt;
			UpdateZombieLook(Scene, Z, 1, FVector3::ZeroVector);
			if (Z.ExplodeTimer <= 0.0f)
			{
				Explode(Scene, T.Position, Z.ExplodeRadius, Z.ExplodeDamage, false, true, Defense);
				Z.bDead      = true;
				Z.Hp         = 0.0f;
				Z.DeathTimer = 0.05f;
				if (Z.Body.IsValid() && Registry.IsValid(Z.Body))
				{
					if (FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Z.Body))
					{
						Sprite->bVisible = false;
					}
				}
			}
			continue;
		}
		const FVector3 Pos  = T.Position;
		const int32    Cell = CellOf(Pos);
		// 크리스탈 발견 → 주변 호출
		if (!Z.bAlerted && CrystalCell >= 0 && Flat(Pos - CrystalPos).Length() < Defense.CrystalDetectRadius)
		{
			Z.bAlerted = true;
			Defense.bCrystalFound = true;
			++Defense.Alerts;
			for (FInfo& Other : List)
			{
				if (Other.Z != &Z && !Other.Z->bDead && !Other.Z->bAlerted && Flat(Other.T->Position - Pos).Length() < Defense.AlertRadius)
				{
					Other.Z->bAlerted = true;
				}
			}
		}
		// 공격 대상 고르기: 보스 어그로 → 플레이어, 일반 좀비는 플레이어가 가까우면 플레이어
		const float ToPlayer = bHasPlayer ? Flat(PlayerPos - Pos).Length() : 1.0e9f;
		const bool  bChase   = Z.bBoss && Z.AggroTimer > 0.0f;
		FVector3    Move     = FVector3::ZeroVector;
		int32       State    = 0;
		auto Strike = [&](auto&& ApplyHit) {
			State = 1;
			if (Z.bExplode)
			{
				Z.ExplodeTimer = 0.6f;
				return;
			}
			if (Z.AttackTimer <= 0.0f)
			{
				Z.AttackTimer = Z.AttackInterval;
				ApplyHit();
			}
		};
		if (bHasPlayer && ((!Z.bBoss && ToPlayer < PlayerReach + Z.BodyRadius) || (bChase && ToPlayer < BossChaseNear + Z.BodyRadius)))
		{
			Move = Flat(PlayerPos - Pos);
			Strike([&] {
				Defense.PlayerDamage += Z.Damage;
				if (Z.SlowOnHit > 0.0f)
				{
					Defense.PlayerSlow = std::max(Defense.PlayerSlow, 1.5f);
				}
			});
		}
		else if (bChase)
		{
			Move = Flat(PlayerPos - Pos).GetNormalized() * Z.Speed * 1.15f;
		}
		else if (Cell >= 0)
		{
			const std::vector<float>& Field = (Z.bAlerted && CrystalCell >= 0) ? FieldCrystal : FieldFarm;
			const int32 X = Cell % Width;
			const int32 Y = Cell / Width;
			int32 Best = Cell;
			float BestValue = Field.empty() ? InfCost : Field[static_cast<size_t>(Cell)];
			static constexpr int32 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
			static constexpr int32 DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
			for (int32 K = 0; K < 8 && !Field.empty(); ++K)
			{
				const int32 NX = X + DX[K];
				const int32 NY = Y + DY[K];
				if (!InGrid(NX, NY))
				{
					continue;
				}
				const int32 Next = NY * Width + NX;
				if (StaticBlocked[static_cast<size_t>(Next)])
				{
					continue;
				}
				if (K >= 4 && (StaticBlocked[static_cast<size_t>(Y * Width + NX)] || StaticBlocked[static_cast<size_t>(NY * Width + X)] ||
				               CellStructure[static_cast<size_t>(Y * Width + NX)].IsValid() || CellStructure[static_cast<size_t>(NY * Width + X)].IsValid()))
				{
					continue;
				}
				if (Field[static_cast<size_t>(Next)] < BestValue - 0.01f)
				{
					BestValue = Field[static_cast<size_t>(Next)];
					Best      = Next;
				}
			}
			const FEntity Blocker = Best != Cell ? CellStructure[static_cast<size_t>(Best)] : NullEntity;
			const FVector3 Target = CellCenter(Best);
			if (Blocker.IsValid() && Registry.IsValid(Blocker))
			{
				// 막는 설치물·크리스탈: 닿으면 친다
				if (Flat(Target - Pos).Length() < Tile * 0.5f + Z.BodyRadius + 25.0f)
				{
					Move = Flat(Target - Pos);
					FFarmStructureComponent& S = Registry.Get<FFarmStructureComponent>(Blocker);
					Strike([&] { DamageStructure(S, Z.Damage * (S.bCrystal ? 1.0f : Z.StructureDamageMul)); });
				}
				else
				{
					Move = Flat(Target - Pos).GetNormalized() * Z.Speed;
				}
			}
			else if (Best == Cell)
			{
				// 목표 칸 도착: 작물이면 먹는다, 아니면 서성임
				if (CropCells[static_cast<size_t>(Cell)])
				{
					State = 1;
					Z.CropTimer += Dt;
					if (Z.CropTimer >= Z.CropEatTime)
					{
						Z.CropTimer = 0.0f;
						CropCells[static_cast<size_t>(Cell)] = 0;
						Defense.CropEvents += std::to_string(Cell % Width) + "," + std::to_string(Cell / Width) + ";";
						bFieldsDirty = true;
					}
				}
				else
				{
					const FVector3 C = CellCenter(Cell);
					Move = Flat(C - Pos) * 2.0f;
				}
			}
			else
			{
				Z.CropTimer = 0.0f;
				Move = Flat(Target - Pos).GetNormalized() * Z.Speed;
			}
		}
		// 밀어내기 (겹치지 않게)
		FVector3 Push = FVector3::ZeroVector;
		for (const FInfo& Other : List)
		{
			if (Other.Z == &Z || Other.Z->bDead)
			{
				continue;
			}
			const FVector3 D = Flat(Pos - Other.T->Position);
			const float    R = Z.BodyRadius + Other.Z->BodyRadius;
			const float    L = D.Length();
			if (L < R && L > 0.01f)
			{
				Push = Push + D / L * (R - L) * 6.0f;
			}
		}
		const FVector3 Step = (State == 1 ? FVector3::ZeroVector : Move) + Push;
		T.Position = T.Position + FVector3(Step.X, Step.Y, 0.0f) * Dt;
		UpdateZombieLook(Scene, Z, State, Move.IsNearlyZero() ? Z.Facing : Move);
	}
	Defense.Alive = Alive;
}

// ---- 덫·포탑
void FFarmDefenseSystem::UpdateTraps(FScene& Scene, FFarmDefenseComponent& Defense, float Dt)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (Defense.Alive <= 0)
	{
		return;
	}
	Registry.View<FTransformComponent, FFarmStructureComponent>().Each([&](FEntity, FTransformComponent& T, FFarmStructureComponent& S) {
		if (S.bDestroyed || S.Damage <= 0.0f)
		{
			return;
		}
		S.Timer = std::max(0.0f, S.Timer - Dt);
		const FVector3 C = T.Position;
		if (S.Kind == "Mine")
		{
			bool bStepped = false;
			Registry.View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity, FTransformComponent& ZT, FFarmZombieComponent& Z) {
				if (!Z.bDead && Flat(ZT.Position - C).Length() < 55.0f + Z.BodyRadius)
				{
					bStepped = true;
				}
			});
			if (bStepped)
			{
				Explode(Scene, C, S.Radius, S.Damage, true, false, Defense);
				S.bDestroyed = true;
				S.Hp         = 0.0f;
				++Defense.TrapHits;
			}
		}
		else if (S.Kind == "Spike")
		{
			if (S.Timer > 0.0f)
			{
				return;
			}
			bool bHit = false;
			Registry.View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity Entity, FTransformComponent& ZT, FFarmZombieComponent& Z) {
				if (!Z.bDead && Flat(ZT.Position - C).Length() < S.Radius + Z.BodyRadius * 0.5f)
				{
					DamageZombie(Scene, Entity, Z, S.Damage, FVector3::ZeroVector, Defense, false);
					DamageStructure(S, 2.0f); // 덫도 닳는다
					bHit = true;
					++Defense.TrapHits;
				}
			});
			if (bHit)
			{
				S.Timer = S.Cooldown;
			}
		}
		else if (S.Kind == "Turret")
		{
			if (S.Timer > 0.0f)
			{
				return;
			}
			float    BestD = S.Range;
			FVector3 BestPos;
			bool     bFound = false;
			Registry.View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity, FTransformComponent& ZT, FFarmZombieComponent& Z) {
				const float D = Flat(ZT.Position - C).Length();
				if (!Z.bDead && D < BestD)
				{
					BestD   = D;
					BestPos = ZT.Position;
					bFound  = true;
				}
			});
			if (bFound)
			{
				S.Timer = S.Cooldown;
				const FVector3 From = C + FVector3(0.0f, 0.0f, 110.0f);
				SpawnProjectile(Scene, From, Flat(BestPos - C).GetNormalized(), 1700.0f, S.Range + 100.0f, S.Damage, 250.0f, false, "Bolt");
				++Defense.TurretShots;
			}
		}
	});
}

// ---- 투사체·효과
void FFarmDefenseSystem::SpawnProjectile(FScene& Scene, const FVector3& From, const FVector3& Dir, float Speed, float Range, float Damage, float Knock,
                                         bool bPlayer, const std::string& Slice)
{
	FRegistry&    Registry = Scene.GetRegistry();
	const FEntity Entity   = Scene.CreateEntity("Projectile");
	FSpriteComponent& Sprite = Registry.Emplace<FSpriteComponent>(Entity);
	Sprite.Sprite       = FxSprite;
	Sprite.Slice        = Slice;
	Sprite.bLit         = false;
	Sprite.bCastShadows = false;
	Sprite.Blend        = ESpriteBlendMode::Masked;
	// 바닥과 나란히 눕혀(Roll -90) 진행 방향으로 돌린다 (그림의 +X = 앞)
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
	FTransformComponent& T = Registry.Get<FTransformComponent>(Entity);
	T.Position = From;
	T.Rotation = FQuat::FromEuler(0.0f, Yaw, -90.0f);
	FProjectile P;
	P.Entity  = Entity;
	P.Pos     = From;
	P.Vel     = Dir * Speed;
	P.Damage  = Damage;
	P.Knock   = Knock;
	P.Life    = Range / std::max(Speed, 1.0f);
	P.bPlayer = bPlayer;
	Projectiles.push_back(P);
}

void FFarmDefenseSystem::SpawnEffect(FScene& Scene, const FVector3& Pos, const char* Flipbook, const char* Slice, float Life, float Scale)
{
	FRegistry&    Registry = Scene.GetRegistry();
	const FEntity Entity   = Scene.CreateEntity("Effect");
	FSpriteComponent& Sprite = Registry.Emplace<FSpriteComponent>(Entity);
	Sprite.Sprite       = FxSprite;
	Sprite.Slice        = Slice;
	Sprite.bLit         = false;
	Sprite.bCastShadows = false;
	Sprite.Blend        = ESpriteBlendMode::Alpha;
	Sprite.Billboard    = 1;
	FFlipbookComponent& Book = Registry.Emplace<FFlipbookComponent>(Entity);
	Book.Flipbook = Flipbook;
	FTransformComponent& T = Registry.Get<FTransformComponent>(Entity);
	T.Position = Pos;
	T.Scale    = FVector3(Scale, Scale, Scale);
	Effects.push_back({ Entity, Life });
}

void FFarmDefenseSystem::UpdateProjectiles(FScene& Scene, FFarmDefenseComponent& Defense, float Dt)
{
	FRegistry& Registry = Scene.GetRegistry();
	for (size_t I = 0; I < Projectiles.size();)
	{
		FProjectile& P = Projectiles[I];
		P.Life -= Dt;
		const FVector3 Prev = P.Pos;
		P.Pos = P.Pos + P.Vel * Dt;
		bool bHit = false;
		Registry.View<FTransformComponent, FFarmZombieComponent>().Each([&](FEntity Entity, FTransformComponent& T, FFarmZombieComponent& Z) {
			if (bHit || Z.bDead)
			{
				return;
			}
			// 선분(Prev → Pos)과 좀비 원 거리
			const FVector3 A  = Flat(Prev);
			const FVector3 AB = Flat(P.Pos) - A;
			const float    L2 = std::max(AB.LengthSquared(), 1e-4f);
			const float    S  = std::clamp(FVector3::Dot(Flat(T.Position) - A, AB) / L2, 0.0f, 1.0f);
			if (Flat(T.Position - (A + AB * S)).Length() < Z.BodyRadius + 18.0f)
			{
				DamageZombie(Scene, Entity, Z, P.Damage, Flat(P.Vel).GetNormalized() * P.Knock, Defense, P.bPlayer);
				if (P.bPlayer)
				{
					++Defense.AttackHits;
				}
				bHit = true;
			}
		});
		// 막는 설치물에 박힘 (플레이어 화살은 벽 너머로 쏠 수 없다)
		const int32 Cell = CellOf(P.Pos);
		const bool  bWall = Cell >= 0 && P.bPlayer && (StaticBlocked[static_cast<size_t>(Cell)] || CellStructure[static_cast<size_t>(Cell)].IsValid());
		if (bHit || bWall || P.Life <= 0.0f || !Registry.IsValid(P.Entity))
		{
			if (Registry.IsValid(P.Entity))
			{
				PendingDestroy.push_back(P.Entity);
			}
			if (bHit)
			{
				SpawnEffect(Scene, P.Pos, "Sprites/FarmBie/Fx_Hit.eflipbook", "Hit0", 0.2f, 1.0f);
			}
			Projectiles[I] = Projectiles.back();
			Projectiles.pop_back();
			continue;
		}
		Registry.Get<FTransformComponent>(P.Entity).Position = P.Pos;
		++I;
	}
}

void FFarmDefenseSystem::UpdateEffects(FScene& Scene, float Dt)
{
	FRegistry& Registry = Scene.GetRegistry();
	for (size_t I = 0; I < Effects.size();)
	{
		Effects[I].Life -= Dt;
		if (Effects[I].Life <= 0.0f || !Registry.IsValid(Effects[I].Entity))
		{
			if (Registry.IsValid(Effects[I].Entity))
			{
				PendingDestroy.push_back(Effects[I].Entity);
			}
			Effects[I] = Effects.back();
			Effects.pop_back();
			continue;
		}
		++I;
	}
}

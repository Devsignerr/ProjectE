#include "Network/NetPlayerSpawner.h"

#include "Network/NetTypes.h"
#include "Network/ReplicationTypes.h"
#include "Scene/Components.h"
#include "Scene/Gameplay.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <format>
#include <vector>

void FNetPlayerSpawner::Begin(FScene& InScene, std::string InPrefabAsset)
{
	End();
	Scene       = &InScene;
	PrefabAsset = std::move(InPrefabAsset);
	// 씬 전용 플레이어 프리팹 (첫 GameModeComponent의 PlayerPrefab)이 있으면 프로젝트 설정보다 우선
	InScene.GetRegistry().View<FGameModeComponent>().Each([this](FEntity, FGameModeComponent& GameMode) {
		if (!GameMode.PlayerPrefab.empty() && PrefabAsset != GameMode.PlayerPrefab)
		{
			E_LOG(LogNet, Display, "씬 전용 플레이어 프리팹: {}", GameMode.PlayerPrefab);
			PrefabAsset = GameMode.PlayerPrefab;
		}
	});
}

void FNetPlayerSpawner::End()
{
	Scene = nullptr;
	PrefabAsset.clear();
	Pawns.clear();
	NextStartIndex = 0;
}

FEntity FNetPlayerSpawner::SpawnPlayer(uint32 PlayerId)
{
	if (Scene == nullptr || PrefabAsset.empty())
	{
		return NullEntity;
	}
	DespawnPlayer(PlayerId);

	std::string   Error;
	const FEntity Pawn = FPrefabLibrary::Get().Instantiate(*Scene, PrefabAsset, NullEntity, &Error);
	if (!Pawn.IsValid())
	{
		E_LOG(LogNet, Error, "플레이어 {} 프리팹 생성 실패 ({}): {}", PlayerId, PrefabAsset, Error);
		return NullEntity;
	}
	FRegistry& Registry = Scene->GetRegistry();
	Registry.GetOrEmplace<FReplicatedComponent>(Pawn).OwnerPlayerId = static_cast<int32>(PlayerId);
	FNameComponent& Name = Registry.GetOrEmplace<FNameComponent>(Pawn);
	Name.Name            = std::format("{} (플레이어 {})", Name.Name, PlayerId);

	// 생성 위치: PlayerStart를 돌아가며 (엔티티 인덱스 순)
	const std::vector<FEntity> Starts = FindPlayerStarts(*Scene);
	FTransformComponent& Transform = Scene->GetTransform(Pawn);
	if (!Starts.empty())
	{
		const FEntity Start = Starts[NextStartIndex++ % Starts.size()];
		Transform.Position  = Scene->GetTransform(Start).Position;
		Transform.Rotation  = Scene->GetTransform(Start).Rotation;
	}
	else
	{
		Transform.Position += FVector3(0.0f, 150.0f * static_cast<float>(Pawns.size()), 0.0f); // 겹치지 않게
	}
	Pawns[PlayerId] = Pawn;
	E_LOG(LogNet, Display, "플레이어 {} 생성: {}", PlayerId, Name.Name);
	return Pawn;
}

void FNetPlayerSpawner::DespawnPlayer(uint32 PlayerId)
{
	const auto Found = Pawns.find(PlayerId);
	if (Found == Pawns.end())
	{
		return;
	}
	if (Scene != nullptr && Scene->GetRegistry().IsValid(Found->second))
	{
		Scene->DestroyEntity(Found->second);
	}
	Pawns.erase(Found);
}

FEntity FNetPlayerSpawner::FindPawn(uint32 PlayerId) const
{
	const auto Found = Pawns.find(PlayerId);
	return Found != Pawns.end() && Scene != nullptr && Scene->GetRegistry().IsValid(Found->second) ? Found->second : NullEntity;
}

std::vector<FEntity> FNetPlayerSpawner::FindPlayerStarts(FScene& Scene)
{
	std::vector<FEntity> Starts;
	Scene.GetRegistry().View<FNameComponent>().Each([&](FEntity Entity, FNameComponent& Component) {
		if (Component.Name == PlayerStartName)
		{
			Starts.push_back(Entity);
		}
	});
	std::sort(Starts.begin(), Starts.end(), [](FEntity A, FEntity B) { return A.Index < B.Index; });
	return Starts;
}

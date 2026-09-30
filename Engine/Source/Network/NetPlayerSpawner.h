#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <string>
#include <unordered_map>

class FScene;

// 서버: 플레이어 입장 → 플레이어 프리팹(.eproject "PlayerPrefab") 생성 + 소유권(FReplicatedComponent::OwnerPlayerId), 퇴장 → 제거.
// 생성 위치: 이름이 "PlayerStart"인 엔티티를 차례로 돌아가며 사용 (없으면 원점에서 플레이어마다 옆으로 비켜서).
// 생성된 엔티티는 복제 엔티티이므로 FReplicationServer가 다음 전송 틱에 클라이언트에게 생성 메시지를 보낸다
class FNetPlayerSpawner
{
public:
	static constexpr const char* PlayerStartName = "PlayerStart";

	// Scene은 이 객체보다 오래 산다 (비소유). PrefabAsset이 비어 있으면 아무것도 만들지 않는다
	void Begin(FScene& InScene, std::string InPrefabAsset);
	void End();

	FEntity SpawnPlayer(uint32 PlayerId); // 실패/프리팹 없음이면 NullEntity
	void    DespawnPlayer(uint32 PlayerId);
	FEntity FindPawn(uint32 PlayerId) const;

private:
	FScene*                              Scene = nullptr;
	std::string                          PrefabAsset;
	std::unordered_map<uint32, FEntity>  Pawns;
	uint32                               NextStartIndex = 0;
};

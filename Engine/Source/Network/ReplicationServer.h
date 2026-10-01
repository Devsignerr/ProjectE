#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"
#include "Network/NetTypes.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class FNetDriver;
class FScene;

// 서버 복제: 복제 엔티티(FReplicatedComponent)의 생성/파괴/컴포넌트 값을 입장한 클라이언트에게 보낸다 (신뢰 채널).
//   - 정적 엔티티(씬 파일): Begin에서 NetId 부여. 클라이언트도 같은 씬을 로드해 같은 NetId를 갖는다
//   - 동적 엔티티: 전송 틱에 NetId가 없는 복제 엔티티를 찾아 생성 메시지를 보낸다.
//     프리팹 인스턴스 루트면 프리팹 경로 + (링크 ID → NetId), 아니면 이름 + 부모 NetId (컴포넌트는 곧바로 상태 메시지로)
//   - 컴포넌트 값: 마지막으로 보낸 바이트와 다르면 그 컴포넌트 전체를 다시 보낸다 (모든 연결 공통 — 신뢰 채널이라 순서·도착 보장)
//   - 트랜스폼: 생성/입장 때만 신뢰 채널로, 이후 변화는 비신뢰 스냅샷(서버 시각 포함)으로 보낸다. 손실에 대비해
//     멈춘 뒤에도 RestResendSeconds 동안 계속 보낸다. 클라이언트가 약간 늦게 보간해 표시한다
//   - 새로 입장한 플레이어: 동적 엔티티 생성 + 모든 복제 엔티티의 현재 값을 한 번에 보낸다
// 전송 주기는 SendRate (기본 30Hz). 게임플레이 틱 뒤에 Tick을 부른다
class FReplicationServer
{
public:
	// Scene/Driver는 이 객체보다 오래 산다 (비소유). 씬 로드 직후, 게임 시작 전에 부른다
	void Begin(FScene& InScene, FNetDriver& InDriver);
	void End();

	void Tick(float DeltaSeconds);
	void OnPlayerJoined(FNetConnectionId Connection); // 불러온 서브 씬 목록 → 동적 엔티티 생성 → 전체 상태

	// 서브 씬 (Phase 31-2): 붙인 직후(같은 프레임, Tick 전) — 클라이언트에 SubSceneLoad를 보내고 하위 복제 엔티티에 구간 NetId를 매긴다
	// (정적 엔티티처럼 파일 값을 보낸 것으로 기록). Unregister는 SubSceneUnload만 보낸다 (엔티티 파괴는 호출자, 파괴 메시지는 다음 Tick)
	void RegisterSubScene(FEntity Root, const std::string& Asset, uint32 InstanceId, const FVector3& Offset);
	void UnregisterSubScene(uint32 InstanceId);

	float SendRate          = 30.0f;
	float RestResendSeconds = 1.0f; // 트랜스폼이 멈춘 뒤에도 이만큼 계속 보낸다 (비신뢰 손실 대비)

	uint32 GetReplicatedCount() const { return static_cast<uint32>(Tracked.size()); }

private:
	enum class ESpawnKind : uint8
	{
		Static,      // 씬 파일 (클라이언트에 이미 있음)
		Entity,      // 스크립트/코드가 만든 엔티티
		Prefab,      // 프리팹 인스턴스 루트
		PrefabChild, // 프리팹 루트와 함께 생성됨
	};

	struct FTracked
	{
		FEntity    Entity;
		ESpawnKind Kind        = ESpawnKind::Static;
		uint32     ParentNetId = 0;
		std::string Name;
		std::string PrefabAsset;
		std::vector<std::pair<std::string, uint32>> PrefabLinks; // 링크 ID → NetId (Prefab만)
		std::unordered_map<std::string, std::vector<uint8>> LastSent; // 컴포넌트 이름 → 마지막으로 보낸 값

		// 트랜스폼 스냅샷
		FVector3 LastPosition;
		FQuat    LastRotation;
		FVector3 LastScale;
		float    LastChangeTime = -1.0e9f; // 서버 시각 (초)
	};

	void DiscoverNewEntities(std::vector<uint32>& OutSpawned);
	void CollectDestroyed(std::vector<uint32>& OutDestroyed);
	// bFull: 바뀌지 않은 컴포넌트도 보낸다 (새 플레이어). bCommit: LastSent 갱신
	std::vector<uint8> BuildStateMessage(const std::vector<uint32>& NetIds, bool bFull, bool bCommit);
	std::vector<uint8> BuildSpawnMessage(const std::vector<uint32>& NetIds) const;
	std::vector<uint32> GetSortedNetIds() const;
	std::vector<uint8>  BuildTransformSnapshot();

	FScene*     Scene  = nullptr;
	FNetDriver* Driver = nullptr;
	std::unordered_map<uint32, FTracked> Tracked;
	struct FSubSceneEntry
	{
		uint32      InstanceId = 0;
		std::string Asset;
		FVector3    Offset;
	};
	std::vector<FSubSceneEntry> SubScenes; // 불러온 서브 씬 (늦은 입장자에게 먼저 보낸다)
	uint32      NextDynamicNetId = 0;
	float       SendAccumulator  = 0.0f;
	float       ServerTime       = 0.0f;
};

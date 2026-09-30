#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

#include <unordered_map>
#include <vector>

class FScene;

// 클라이언트 복제: 서버의 생성/파괴/컴포넌트 값 메시지를 씬에 적용한다 (형식은 ReplicationServer.cpp).
// 씬 파일의 복제 엔티티는 Begin에서 서버와 같은 정적 NetId를 받는다 (서버와 같은 씬을 로드한 직후에 부를 것).
// 생성이나 에셋 경로가 있는 컴포넌트가 바뀌면 ConsumeAssetsChanged()가 true — 앱이 에셋 해석(FSceneAssetResolver)을 다시 한다
class FReplicationClient
{
public:
	void Begin(FScene& InScene); // Scene은 이 객체보다 오래 산다 (비소유)
	void End();

	// FNetDriver::OnGameMessage에서 받은 메시지. 복제 메시지가 아니면 false
	bool HandleMessage(const std::vector<uint8>& Message);

	bool    ConsumeAssetsChanged();
	FEntity FindEntity(uint32 NetId) const; // 없거나 파괴됐으면 NullEntity

private:
	void ApplySpawn(const std::vector<uint8>& Message);
	void ApplyDestroy(const std::vector<uint8>& Message);
	void ApplyState(const std::vector<uint8>& Message);

	FScene*                              Scene = nullptr;
	std::unordered_map<uint32, FEntity>  Entities; // NetId → 엔티티
	bool                                 bAssetsChanged = false;
};

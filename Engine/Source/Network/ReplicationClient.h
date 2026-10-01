#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class FScene;

// 클라이언트 복제: 서버의 생성/파괴/컴포넌트 값 메시지를 씬에 적용한다 (형식은 ReplicationServer.cpp).
// 씬 파일의 복제 엔티티는 Begin에서 서버와 같은 정적 NetId를 받는다 (서버와 같은 씬을 로드한 직후에 부를 것).
// 생성이나 에셋 경로가 있는 컴포넌트가 바뀌면 ConsumeAssetsChanged()가 true — 앱이 에셋 해석(FSceneAssetResolver)을 다시 한다.
// 트랜스폼 스냅샷은 버퍼에 쌓고 Update에서 (서버 시각 추정 - InterpolationDelay) 시점으로 보간해 쓴다 (외삽 없음)
class FReplicationClient
{
public:
	void Begin(FScene& InScene); // Scene은 이 객체보다 오래 산다 (비소유)
	void End();

	// FNetDriver::OnGameMessage에서 받은 메시지. 복제 메시지가 아니면 false
	bool HandleMessage(const std::vector<uint8>& Message);
	// 매 프레임 (메시지 처리 뒤, 트랜스폼 갱신 전): 서버 시각 추정 진행 + 트랜스폼 보간
	void Update(float DeltaSeconds);

	float InterpolationDelay = 0.1f; // 초. 스냅샷 간격(1/30초)의 3배 — 한두 개 손실돼도 보간할 두 점이 있다

	bool    ConsumeAssetsChanged();
	FEntity FindEntity(uint32 NetId) const; // 없거나 파괴됐으면 NullEntity
	// 거짓을 돌려주는 엔티티에는 스냅샷 보간 트랜스폼을 쓰지 않는다 (소유 클라이언트가 예측하는 캐릭터·물리 바디 — FGameWorld::IsPredicted)
	void SetTransformFilter(std::function<bool(FEntity)> ShouldApply) { TransformFilter = std::move(ShouldApply); }

	// 받은 스냅샷 (로컬 트랜스폼, 서버 시각순). 물리 예측이 서버 상태를 현재로 외삽할 때 읽는다
	struct FTransformSample
	{
		float    ServerTime = 0.0f;
		FVector3 Position;
		FQuat    Rotation;
		FVector3 Scale;
	};
	bool  HasServerClock() const { return bClockValid; }
	float GetServerClock() const { return ServerClock; } // 추정한 현재 서버 시각 (가장 최근 스냅샷 시각을 따라간다)
	float GetLatestSnapshotTime() const { return LatestSnapshotTime; } // 받은 가장 최근 스냅샷의 서버 시각 (없으면 음수)
	const std::deque<FTransformSample>* FindTransformSamples(FEntity Entity) const; // 없으면 nullptr
	// Update가 쓰는 것과 같은 보간 값 (필터와 무관). 스냅샷이 없으면 false
	bool SampleTransform(FEntity Entity, FVector3& OutPosition, FQuat& OutRotation) const;

	// 서버가 서브 씬을 불러오거나 내릴 때 (SubSceneLoad/Unload, HandleMessage 안에서 곧바로). Load는 같은 파일을 씬에 붙이고 루트를 돌려준다
	// (실패 NullEntity) → 그 하위 복제 엔티티에 서버와 같은 구간 NetId. FGameWorld::SetReplicationClient가 연결한다. Begin/End에도 유지
	struct FSubSceneHooks
	{
		std::function<FEntity(const std::string& Asset, uint32 InstanceId, const FVector3& Offset)> Load;
		std::function<void(uint32 InstanceId)>                                                     Unload;
	};
	void SetSubSceneHooks(FSubSceneHooks Hooks) { SubSceneHooks = std::move(Hooks); }

private:
	void ApplySubSceneLoad(const std::vector<uint8>& Message);
	void ApplySubSceneUnload(const std::vector<uint8>& Message);
	FSubSceneHooks SubSceneHooks;
	void ApplySpawn(const std::vector<uint8>& Message);
	void ApplyDestroy(const std::vector<uint8>& Message);
	void ApplyState(const std::vector<uint8>& Message);
	void ApplyTransformSnapshot(const std::vector<uint8>& Message);
	static FTransformSample Interpolate(const std::deque<FTransformSample>& Buffer, float RenderTime); // Buffer는 비어 있지 않다

	FScene*                              Scene = nullptr;
	std::unordered_map<uint32, FEntity>  Entities; // NetId → 엔티티
	bool                                 bAssetsChanged = false;

	std::unordered_map<uint32, std::deque<FTransformSample>> TransformBuffers; // NetId → 시각순 스냅샷
	std::function<bool(FEntity)>                             TransformFilter;
	float                                                    ServerClock  = 0.0f; // 추정한 현재 서버 시각
	bool                                                     bClockValid  = false;
	float                                                    LatestSnapshotTime = -1.0f;
};

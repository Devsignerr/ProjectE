#pragma once

#include "Audio/AudioEngine.h"
#include "Core/ECS/Entity.h"

#include <filesystem>
#include <string>
#include <unordered_map>

class FScene;

// FAudioSourceComponent ↔ FAudioEngine 사운드 동기화 시스템.
//   - 새 소스: 클립 로드 → bPlayOnStart면 재생
//   - 매 프레임: 속성(볼륨/피치/루프/거리)과 월드 위치 반영 (FScene::UpdateTransforms 이후 호출)
//   - 컴포넌트/엔티티가 사라지면 사운드 해제, ClipAsset이 바뀌면 다시 로드
class FAudioSystem
{
public:
	// 청자는 호출 전에 Engine.SetListener로 설정한다
	void Update(FScene& Scene, FAudioEngine& Engine, const std::filesystem::path& ContentDirectory);

	// 모든 소스 사운드 해제 (씬 교체, 플레이 정지)
	void Reset(FAudioEngine& Engine);

	// 게임플레이 제어 (Update로 사운드가 만들어진 엔티티만 유효)
	void Play(FAudioEngine& Engine, FEntity Entity);
	void Stop(FAudioEngine& Engine, FEntity Entity);
	FSoundHandle GetSound(FEntity Entity) const;

	uint32 GetSourceCount() const { return static_cast<uint32>(Sources.size()); }

private:
	struct FSourceState
	{
		FSoundHandle Sound;     // 로드 실패 시 무효 (ClipAsset이 바뀔 때까지 재시도하지 않음)
		std::string  ClipAsset;
		uint64       LastSeenFrame = 0;
	};

	std::unordered_map<FEntity, FSourceState> Sources;
	uint64                                    FrameCounter = 0;
};

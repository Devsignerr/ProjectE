#pragma once

#include "Audio/AudioMath.h"
#include "Core/CoreTypes.h"
#include "Core/Log.h"

#include <filesystem>
#include <memory>
#include <vector>

E_DECLARE_ENGINE_LOG_CATEGORY(LogAudio)

struct FAudioEngineDesc
{
	bool   bNoDevice  = false; // 출력 장치 없이 동작 (테스트: ReadFrames로 믹스 결과를 직접 읽음)
	uint32 Channels   = 2;     // bNoDevice일 때만 사용 (장치 모드는 장치 기본값)
	uint32 SampleRate = 48000;
};

// 사운드 인스턴스 핸들 (0 = 무효)
struct FSoundHandle
{
	uint32 Id = 0;

	bool IsValid() const { return Id != 0; }
	bool operator==(const FSoundHandle&) const = default;
};

struct FSoundDesc
{
	float Volume      = 1.0f;
	float Pitch       = 1.0f;
	bool  bLoop       = false;
	bool  bSpatial    = true;
	float MinDistance = 100.0f;  // cm
	float MaxDistance = 5000.0f; // cm
};

// miniaudio 엔진 래퍼: 장치 출력, 디코딩(리소스 매니저 캐시), 3D 공간화.
// 장치가 없거나 초기화에 실패하면 IsInitialized() == false이고 모든 호출은 무시된다 (게임은 무음으로 계속).
class FAudioEngine
{
public:
	FAudioEngine();
	~FAudioEngine();

	FAudioEngine(const FAudioEngine&)            = delete;
	FAudioEngine& operator=(const FAudioEngine&) = delete;

	bool Init(const FAudioEngineDesc& Desc = FAudioEngineDesc{});
	void Shutdown();
	bool IsInitialized() const;

	void SetMasterVolume(float Volume);
	void SetListener(const FAudioListener& InListener);
	const FAudioListener& GetListener() const { return Listener; }

	// 사운드 인스턴스 (파일 디코딩 결과는 엔진이 경로별로 캐시)
	FSoundHandle CreateSound(const std::filesystem::path& Path, const FSoundDesc& Desc = FSoundDesc{});
	void         DestroySound(FSoundHandle Handle);
	void         DestroyAllSounds();
	bool         IsValid(FSoundHandle Handle) const;

	void Play(FSoundHandle Handle);        // 처음부터 재생
	void Stop(FSoundHandle Handle);        // 정지 + 처음으로 되감기
	bool IsPlaying(FSoundHandle Handle) const;
	bool IsAtEnd(FSoundHandle Handle) const;

	void ApplyDesc(FSoundHandle Handle, const FSoundDesc& Desc);
	void SetWorldPosition(FSoundHandle Handle, const FVector3& WorldPosition); // 현재 리스너 기준으로 변환해 설정

	// 발사 후 잊기 (핸들 없음, 비공간). 효과음 미리 듣기 등. 끝난 사운드는 Update에서 정리
	void PlayOneShot(const std::filesystem::path& Path);
	// 음량·피치 배율, WorldPosition이 있으면 3D 공간화 (청자 기준으로 변환)
	void PlayOneShot(const std::filesystem::path& Path, float Volume, float Pitch, const FVector3* WorldPosition);

	// 프레임마다 한 번: 끝난 원샷 정리
	void Update();

	// bNoDevice 모드 전용: 믹스 결과를 인터리브 float로 읽는다 (FrameCount * Channels개)
	bool ReadFrames(float* OutFrames, uint32 FrameCount);
	uint32 GetChannels() const;

	uint32 GetSoundCount() const;

private:
	struct FImpl;
	struct FSound;

	FSound*       FindSound(FSoundHandle Handle);
	const FSound* FindSound(FSoundHandle Handle) const;

	std::unique_ptr<FImpl> Impl;
	FAudioListener         Listener;
};

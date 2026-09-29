#include "Audio/AudioEngine.h"

#include "Core/Assert.h"
#include "Core/StringConv.h"

#pragma warning(push, 0)
#include <miniaudio.h>
#pragma warning(pop)

#include <algorithm>
#include <unordered_map>

E_DEFINE_LOG_CATEGORY(LogAudio, Log)

struct FAudioEngine::FSound
{
	ma_sound Sound{};
	bool     bSpatial = true;
	bool     bOneShot = false;
};

struct FAudioEngine::FImpl
{
	ma_engine Engine{};
	bool      bInitialized = false;
	bool      bNoDevice    = false;
	uint32    NextSoundId  = 1;

	// ma_sound는 초기화 후 주소가 바뀌면 안 되므로 unique_ptr로 보관
	std::unordered_map<uint32, std::unique_ptr<FSound>> Sounds;
};

FAudioEngine::FAudioEngine()
	: Impl(std::make_unique<FImpl>())
{
}

FAudioEngine::~FAudioEngine()
{
	Shutdown();
}

bool FAudioEngine::Init(const FAudioEngineDesc& Desc)
{
	E_CHECKF(!Impl->bInitialized, "FAudioEngine::Init 중복 호출");

	ma_engine_config Config = ma_engine_config_init();
	Config.listenerCount    = 1;
	if (Desc.bNoDevice)
	{
		Config.noDevice   = MA_TRUE;
		Config.channels   = Desc.Channels;
		Config.sampleRate = Desc.SampleRate;
	}

	const ma_result Result = ma_engine_init(&Config, &Impl->Engine);
	if (Result != MA_SUCCESS)
	{
		E_LOG(LogAudio, Warning, "오디오 엔진 초기화 실패 ({}): 무음으로 계속합니다", ma_result_description(Result));
		return false;
	}
	Impl->bInitialized = true;
	Impl->bNoDevice    = Desc.bNoDevice;

	// 청자는 원점에서 -Z를 본다. 사운드 위치는 AudioMath::ToListenerSpace로 청자 기준 좌표로 넘긴다
	ma_engine_listener_set_position(&Impl->Engine, 0, 0.0f, 0.0f, 0.0f);
	ma_engine_listener_set_direction(&Impl->Engine, 0, 0.0f, 0.0f, -1.0f);
	ma_engine_listener_set_world_up(&Impl->Engine, 0, 0.0f, 1.0f, 0.0f);

	E_LOG(LogAudio, Display, "오디오 엔진 초기화 완료: {} Hz, {} 채널{}", ma_engine_get_sample_rate(&Impl->Engine),
	      ma_engine_get_channels(&Impl->Engine), Desc.bNoDevice ? " (장치 없음)" : "");
	return true;
}

void FAudioEngine::Shutdown()
{
	if (!Impl || !Impl->bInitialized)
	{
		return;
	}
	DestroyAllSounds();
	ma_engine_uninit(&Impl->Engine);
	Impl->bInitialized = false;
}

bool FAudioEngine::IsInitialized() const
{
	return Impl->bInitialized;
}

void FAudioEngine::SetMasterVolume(float Volume)
{
	if (Impl->bInitialized)
	{
		ma_engine_set_volume(&Impl->Engine, std::max(Volume, 0.0f));
	}
}

void FAudioEngine::SetListener(const FAudioListener& InListener)
{
	Listener = InListener;
}

FSoundHandle FAudioEngine::CreateSound(const std::filesystem::path& Path, const FSoundDesc& Desc)
{
	if (!Impl->bInitialized)
	{
		return {};
	}

	auto Sound = std::make_unique<FSound>();
	// DECODE: 전체를 메모리에 디코딩 (리소스 매니저가 같은 경로를 공유 캐시). 긴 음악은 후속으로 STREAM 옵션
	const ma_result Result = ma_sound_init_from_file_w(&Impl->Engine, Path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &Sound->Sound);
	if (Result != MA_SUCCESS)
	{
		E_LOG(LogAudio, Warning, "사운드 로드 실패 ({}): {}", ma_result_description(Result), FStringConv::ToUtf8(Path.wstring()));
		return {};
	}

	ma_sound_set_doppler_factor(&Sound->Sound, 0.0f);
	ma_sound_set_attenuation_model(&Sound->Sound, ma_attenuation_model_inverse);

	float LengthSeconds = 0.0f;
	ma_sound_get_length_in_seconds(&Sound->Sound, &LengthSeconds);
	E_LOG(LogAudio, Log, "사운드 로드: {} ({:.2f}초)", FStringConv::ToUtf8(Path.filename().wstring()), LengthSeconds);

	const FSoundHandle Handle{ Impl->NextSoundId++ };
	Impl->Sounds.emplace(Handle.Id, std::move(Sound));
	ApplyDesc(Handle, Desc);
	return Handle;
}

void FAudioEngine::DestroySound(FSoundHandle Handle)
{
	const auto It = Impl->Sounds.find(Handle.Id);
	if (It != Impl->Sounds.end())
	{
		ma_sound_uninit(&It->second->Sound);
		Impl->Sounds.erase(It);
	}
}

void FAudioEngine::DestroyAllSounds()
{
	for (auto& [Id, Sound] : Impl->Sounds)
	{
		ma_sound_uninit(&Sound->Sound);
	}
	Impl->Sounds.clear();
}

bool FAudioEngine::IsValid(FSoundHandle Handle) const
{
	return FindSound(Handle) != nullptr;
}

void FAudioEngine::Play(FSoundHandle Handle)
{
	if (FSound* Sound = FindSound(Handle))
	{
		ma_sound_seek_to_pcm_frame(&Sound->Sound, 0);
		ma_sound_start(&Sound->Sound);
	}
}

void FAudioEngine::Stop(FSoundHandle Handle)
{
	if (FSound* Sound = FindSound(Handle))
	{
		ma_sound_stop(&Sound->Sound);
		ma_sound_seek_to_pcm_frame(&Sound->Sound, 0);
	}
}

bool FAudioEngine::IsPlaying(FSoundHandle Handle) const
{
	const FSound* Sound = FindSound(Handle);
	return Sound && ma_sound_is_playing(&Sound->Sound);
}

bool FAudioEngine::IsAtEnd(FSoundHandle Handle) const
{
	const FSound* Sound = FindSound(Handle);
	return Sound && ma_sound_at_end(&Sound->Sound);
}

void FAudioEngine::ApplyDesc(FSoundHandle Handle, const FSoundDesc& Desc)
{
	FSound* Sound = FindSound(Handle);
	if (!Sound)
	{
		return;
	}
	ma_sound_set_volume(&Sound->Sound, std::max(Desc.Volume, 0.0f));
	ma_sound_set_pitch(&Sound->Sound, std::max(Desc.Pitch, 0.01f));
	ma_sound_set_looping(&Sound->Sound, Desc.bLoop ? MA_TRUE : MA_FALSE);
	ma_sound_set_spatialization_enabled(&Sound->Sound, Desc.bSpatial ? MA_TRUE : MA_FALSE);
	ma_sound_set_min_distance(&Sound->Sound, std::max(Desc.MinDistance, 1.0f));
	ma_sound_set_max_distance(&Sound->Sound, std::max(Desc.MaxDistance, Desc.MinDistance + 1.0f));
	Sound->bSpatial = Desc.bSpatial;
}

void FAudioEngine::SetWorldPosition(FSoundHandle Handle, const FVector3& WorldPosition)
{
	FSound* Sound = FindSound(Handle);
	if (!Sound || !Sound->bSpatial)
	{
		return;
	}
	const FVector3 Local = AudioMath::ToListenerSpace(Listener, WorldPosition);
	ma_sound_set_position(&Sound->Sound, Local.X, Local.Y, Local.Z);
}

void FAudioEngine::PlayOneShot(const std::filesystem::path& Path)
{
	FSoundDesc Desc;
	Desc.bSpatial = false;
	const FSoundHandle Handle = CreateSound(Path, Desc);
	if (FSound* Sound = FindSound(Handle))
	{
		Sound->bOneShot = true;
		ma_sound_start(&Sound->Sound);
	}
}

void FAudioEngine::Update()
{
	// 재생이 끝난 원샷 사운드 정리
	for (auto It = Impl->Sounds.begin(); It != Impl->Sounds.end();)
	{
		if (It->second->bOneShot && ma_sound_at_end(&It->second->Sound))
		{
			ma_sound_uninit(&It->second->Sound);
			It = Impl->Sounds.erase(It);
		}
		else
		{
			++It;
		}
	}
}

bool FAudioEngine::ReadFrames(float* OutFrames, uint32 FrameCount)
{
	if (!Impl->bInitialized || !Impl->bNoDevice)
	{
		return false;
	}
	ma_uint64 FramesRead = 0;
	return ma_engine_read_pcm_frames(&Impl->Engine, OutFrames, FrameCount, &FramesRead) == MA_SUCCESS;
}

uint32 FAudioEngine::GetChannels() const
{
	return Impl->bInitialized ? ma_engine_get_channels(&Impl->Engine) : 0;
}

uint32 FAudioEngine::GetSoundCount() const
{
	return static_cast<uint32>(Impl->Sounds.size());
}

FAudioEngine::FSound* FAudioEngine::FindSound(FSoundHandle Handle)
{
	const auto It = Impl->Sounds.find(Handle.Id);
	return It != Impl->Sounds.end() ? It->second.get() : nullptr;
}

const FAudioEngine::FSound* FAudioEngine::FindSound(FSoundHandle Handle) const
{
	const auto It = Impl->Sounds.find(Handle.Id);
	return It != Impl->Sounds.end() ? It->second.get() : nullptr;
}

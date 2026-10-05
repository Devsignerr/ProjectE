#include "Audio/AudioEngine.h"

#include "Core/Assert.h"
#include "Core/FileSystem.h"
#include "Core/StringConv.h"

#pragma warning(push, 0)
#include <miniaudio.h>
#pragma warning(pop)

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

E_DEFINE_LOG_CATEGORY(LogAudio, Log)

namespace
{
	// miniaudio 파일 읽기를 FFileSystem(pak → 디스크)으로: 열 때 파일 전체를 메모리로 읽는다 (DECODE 로드라 어차피 전부 읽음)
	struct FMemoryFile
	{
		std::vector<uint8> Bytes;
		size_t             Cursor = 0;
	};

	ma_result VfsOpenW(ma_vfs*, const wchar_t* FilePath, ma_uint32 OpenMode, ma_vfs_file* OutFile)
	{
		if ((OpenMode & MA_OPEN_MODE_WRITE) != 0)
		{
			return MA_ACCESS_DENIED;
		}
		auto File = std::make_unique<FMemoryFile>();
		if (!FFileSystem::ReadFile(FilePath, File->Bytes))
		{
			return MA_DOES_NOT_EXIST;
		}
		*OutFile = File.release();
		return MA_SUCCESS;
	}

	ma_result VfsOpen(ma_vfs* Vfs, const char* FilePath, ma_uint32 OpenMode, ma_vfs_file* OutFile)
	{
		return VfsOpenW(Vfs, FStringConv::ToWide(FilePath).c_str(), OpenMode, OutFile);
	}

	ma_result VfsClose(ma_vfs*, ma_vfs_file File)
	{
		delete static_cast<FMemoryFile*>(File);
		return MA_SUCCESS;
	}

	ma_result VfsRead(ma_vfs*, ma_vfs_file File, void* Destination, size_t SizeInBytes, size_t* OutBytesRead)
	{
		FMemoryFile* Memory = static_cast<FMemoryFile*>(File);
		const size_t Count  = std::min(SizeInBytes, Memory->Bytes.size() - Memory->Cursor);
		std::memcpy(Destination, Memory->Bytes.data() + Memory->Cursor, Count);
		Memory->Cursor += Count;
		if (OutBytesRead != nullptr)
		{
			*OutBytesRead = Count;
		}
		return Count == 0 && SizeInBytes > 0 ? MA_AT_END : MA_SUCCESS;
	}

	ma_result VfsWrite(ma_vfs*, ma_vfs_file, const void*, size_t, size_t*)
	{
		return MA_ACCESS_DENIED;
	}

	ma_result VfsSeek(ma_vfs*, ma_vfs_file File, ma_int64 Offset, ma_seek_origin Origin)
	{
		FMemoryFile*   Memory = static_cast<FMemoryFile*>(File);
		const ma_int64 Base   = Origin == ma_seek_origin_start ? 0 : Origin == ma_seek_origin_current ? static_cast<ma_int64>(Memory->Cursor)
		                                                                                               : static_cast<ma_int64>(Memory->Bytes.size());
		const ma_int64 Target = Base + Offset;
		if (Target < 0 || Target > static_cast<ma_int64>(Memory->Bytes.size()))
		{
			return MA_BAD_SEEK;
		}
		Memory->Cursor = static_cast<size_t>(Target);
		return MA_SUCCESS;
	}

	ma_result VfsTell(ma_vfs*, ma_vfs_file File, ma_int64* OutCursor)
	{
		*OutCursor = static_cast<ma_int64>(static_cast<FMemoryFile*>(File)->Cursor);
		return MA_SUCCESS;
	}

	ma_result VfsInfo(ma_vfs*, ma_vfs_file File, ma_file_info* OutInfo)
	{
		OutInfo->sizeInBytes = static_cast<FMemoryFile*>(File)->Bytes.size();
		return MA_SUCCESS;
	}

	// ma_vfs*는 콜백 표로 시작하는 객체를 가리킨다
	ma_vfs_callbacks GContentVfs = { &VfsOpen, &VfsOpenW, &VfsClose, &VfsRead, &VfsWrite, &VfsSeek, &VfsTell, &VfsInfo };
} // namespace

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

	ma_engine_config Config    = ma_engine_config_init();
	Config.listenerCount       = 1;
	Config.pResourceManagerVFS = &GContentVfs; // 사운드 파일은 FFileSystem으로 (pak 지원)
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

void FAudioEngine::PlayOneShot(const std::filesystem::path& Path, float Volume, float Pitch, const FVector3* WorldPosition)
{
	FSoundDesc Desc;
	Desc.Volume   = Volume;
	Desc.Pitch    = Pitch;
	Desc.bSpatial = WorldPosition != nullptr;
	const FSoundHandle Handle = CreateSound(Path, Desc);
	if (FSound* Sound = FindSound(Handle))
	{
		Sound->bOneShot = true;
		if (WorldPosition != nullptr)
		{
			SetWorldPosition(Handle, *WorldPosition);
		}
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

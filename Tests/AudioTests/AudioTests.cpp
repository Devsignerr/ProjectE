#include "Audio/AudioComponents.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioMath.h"
#include "Audio/AudioReflection.h"
#include "Audio/AudioSystem.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
	constexpr uint32 SampleRate = 48000;

	// 테스트용 16비트 모노 사인파 WAV 작성
	std::filesystem::path WriteSineWav(const char* FileName, float Frequency, float Seconds)
	{
		const std::filesystem::path Directory = std::filesystem::temp_directory_path() / "ProjectEAudioTests";
		std::filesystem::create_directories(Directory);
		const std::filesystem::path Path = Directory / FileName;

		const uint32 FrameCount = static_cast<uint32>(Seconds * static_cast<float>(SampleRate));
		const uint32 DataBytes  = FrameCount * 2;

		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		auto Write32 = [&](uint32 Value) { File.write(reinterpret_cast<const char*>(&Value), 4); };
		auto Write16 = [&](uint16 Value) { File.write(reinterpret_cast<const char*>(&Value), 2); };
		File.write("RIFF", 4);
		Write32(36 + DataBytes);
		File.write("WAVEfmt ", 8);
		Write32(16);
		Write16(1); // PCM
		Write16(1); // 모노
		Write32(SampleRate);
		Write32(SampleRate * 2);
		Write16(2);
		Write16(16);
		File.write("data", 4);
		Write32(DataBytes);
		for (uint32 Index = 0; Index < FrameCount; ++Index)
		{
			const float Sample = 0.5f * std::sin(2.0f * FMath::Pi * Frequency * static_cast<float>(Index) / static_cast<float>(SampleRate));
			Write16(static_cast<uint16>(static_cast<int16>(Sample * 32767.0f)));
		}
		return Path;
	}

	struct FChannelEnergy
	{
		float Left  = 0.0f;
		float Right = 0.0f;
		float Total() const { return Left + Right; }
	};

	FChannelEnergy MixEnergy(FAudioEngine& Engine, uint32 FrameCount)
	{
		std::vector<float> Frames(static_cast<size_t>(FrameCount) * 2);
		Engine.ReadFrames(Frames.data(), FrameCount);
		FChannelEnergy Energy;
		for (uint32 Index = 0; Index < FrameCount; ++Index)
		{
			Energy.Left  += Frames[Index * 2] * Frames[Index * 2];
			Energy.Right += Frames[Index * 2 + 1] * Frames[Index * 2 + 1];
		}
		return Energy;
	}

	FAudioEngineDesc MakeTestDesc()
	{
		FAudioEngineDesc Desc;
		Desc.bNoDevice  = true;
		Desc.Channels   = 2;
		Desc.SampleRate = SampleRate;
		return Desc;
	}
}

E_TEST(AudioMath_ListenerSpaceAxes)
{
	FAudioListener Listener; // 원점, +X를 보고 +Z가 위
	const float    Tol = 1.0e-4f;
	// miniaudio 공간: +X 오른쪽, +Y 위, -Z 앞
	E_EXPECT_EQUALS(AudioMath::ToListenerSpace(Listener, FVector3(100, 0, 0)), FVector3(0, 0, -100), Tol);
	E_EXPECT_EQUALS(AudioMath::ToListenerSpace(Listener, FVector3(0, 100, 0)), FVector3(100, 0, 0), Tol);
	E_EXPECT_EQUALS(AudioMath::ToListenerSpace(Listener, FVector3(0, 0, 100)), FVector3(0, 100, 0), Tol);
}

E_TEST(AudioMath_ListenerSpaceFollowsListener)
{
	// 청자가 (100,0,0)에서 +Y를 보면 오른쪽은 Cross(Up, Forward) = Cross(Z, Y) = -X
	FAudioListener Listener;
	Listener.Position = FVector3(100, 0, 0);
	Listener.Forward  = FVector3::RightVector;
	const float Tol   = 1.0e-4f;
	E_EXPECT_EQUALS(AudioMath::ToListenerSpace(Listener, FVector3(50, 0, 0)), FVector3(50, 0, 0), Tol);  // -X 쪽 = 청자의 오른쪽
	E_EXPECT_EQUALS(AudioMath::ToListenerSpace(Listener, FVector3(100, 30, 0)), FVector3(0, 0, -30), Tol); // 정면
}

E_TEST(AudioEngine_NoDeviceMixesSound)
{
	FAudioEngine Engine;
	E_EXPECT_TRUE(Engine.Init(MakeTestDesc()));
	E_EXPECT_EQ(Engine.GetChannels(), 2u);

	FSoundDesc Desc;
	Desc.bSpatial = false;
	const FSoundHandle Sound = Engine.CreateSound(WriteSineWav("Tone.wav", 440.0f, 0.5f), Desc);
	E_EXPECT_TRUE(Sound.IsValid());

	E_EXPECT_NEAR(MixEnergy(Engine, 1024).Total(), 0.0f, 1.0e-6f); // 재생 전 무음
	Engine.Play(Sound);
	E_EXPECT_TRUE(Engine.IsPlaying(Sound));
	E_EXPECT_TRUE(MixEnergy(Engine, 1024).Total() > 1.0f);

	Engine.Stop(Sound);
	E_EXPECT_FALSE(Engine.IsPlaying(Sound));
	E_EXPECT_NEAR(MixEnergy(Engine, 1024).Total(), 0.0f, 1.0e-6f);

	Engine.DestroySound(Sound);
	E_EXPECT_FALSE(Engine.IsValid(Sound));
	E_EXPECT_EQ(Engine.GetSoundCount(), 0u);
}

E_TEST(AudioEngine_MissingFileFailsGracefully)
{
	FAudioEngine Engine;
	E_EXPECT_TRUE(Engine.Init(MakeTestDesc()));
	E_EXPECT_FALSE(Engine.CreateSound(L"Z:/존재하지않음/없는파일.wav").IsValid());
	E_EXPECT_EQ(Engine.GetSoundCount(), 0u);
}

E_TEST(AudioEngine_UninitializedIgnoresCalls)
{
	FAudioEngine Engine; // Init 없이
	E_EXPECT_FALSE(Engine.IsInitialized());
	const FSoundHandle Sound = Engine.CreateSound(WriteSineWav("Tone.wav", 440.0f, 0.5f));
	E_EXPECT_FALSE(Sound.IsValid());
	Engine.Play(Sound);
	Engine.Update();
	E_EXPECT_FALSE(Engine.IsPlaying(Sound));
}

E_TEST(AudioEngine_SpatialPanning)
{
	FAudioEngine Engine;
	E_EXPECT_TRUE(Engine.Init(MakeTestDesc()));

	const FSoundHandle Sound = Engine.CreateSound(WriteSineWav("Tone.wav", 440.0f, 0.5f));
	E_EXPECT_TRUE(Sound.IsValid());
	Engine.SetWorldPosition(Sound, FVector3(0.0f, 300.0f, 0.0f)); // 엔진 +Y = 청자의 오른쪽
	Engine.Play(Sound);
	MixEnergy(Engine, 2048); // 공간화 스무딩 안정화
	const FChannelEnergy Right = MixEnergy(Engine, 4096);
	E_EXPECT_TRUE(Right.Right > Right.Left * 2.0f);

	Engine.SetWorldPosition(Sound, FVector3(0.0f, -300.0f, 0.0f)); // 왼쪽
	MixEnergy(Engine, 4096);
	const FChannelEnergy Left = MixEnergy(Engine, 4096);
	E_EXPECT_TRUE(Left.Left > Left.Right * 2.0f);
}

E_TEST(AudioEngine_DistanceAttenuation)
{
	FAudioEngine Engine;
	E_EXPECT_TRUE(Engine.Init(MakeTestDesc()));

	FSoundDesc Desc;
	Desc.bLoop       = true;
	Desc.MinDistance = 100.0f;
	Desc.MaxDistance = 100000.0f;
	const FSoundHandle Sound = Engine.CreateSound(WriteSineWav("Tone.wav", 440.0f, 0.5f), Desc);
	Engine.SetWorldPosition(Sound, FVector3(100.0f, 0.0f, 0.0f));
	Engine.Play(Sound);
	MixEnergy(Engine, 2048);
	const float Near = MixEnergy(Engine, 4096).Total();

	Engine.SetWorldPosition(Sound, FVector3(2000.0f, 0.0f, 0.0f)); // 20배 거리
	MixEnergy(Engine, 4096);
	const float Far = MixEnergy(Engine, 4096).Total();
	E_EXPECT_TRUE(Far < Near * 0.1f);
	E_EXPECT_TRUE(Far > 0.0f);
}

E_TEST(AudioSystem_SyncsComponents)
{
	FAudioEngine Engine;
	E_EXPECT_TRUE(Engine.Init(MakeTestDesc()));
	const std::filesystem::path Clip = WriteSineWav("Loop.wav", 220.0f, 0.25f);

	FScene  Scene;
	FEntity Speaker = Scene.CreateEntity("Speaker");
	FAudioSourceComponent& Source = Scene.GetRegistry().Emplace<FAudioSourceComponent>(Speaker);
	Source.ClipAsset = Clip.filename().string();
	Source.bLoop     = true;
	Source.bSpatial  = false;
	Scene.UpdateTransforms();

	FAudioSystem System;
	System.Update(Scene, Engine, Clip.parent_path());
	E_EXPECT_EQ(System.GetSourceCount(), 1u);
	E_EXPECT_TRUE(Engine.IsPlaying(System.GetSound(Speaker)));
	E_EXPECT_TRUE(MixEnergy(Engine, 1024).Total() > 1.0f);

	// 볼륨 0 반영
	Scene.GetRegistry().Get<FAudioSourceComponent>(Speaker).Volume = 0.0f;
	System.Update(Scene, Engine, Clip.parent_path());
	MixEnergy(Engine, 1024);
	E_EXPECT_NEAR(MixEnergy(Engine, 1024).Total(), 0.0f, 1.0e-4f);

	// 컴포넌트 제거 → 사운드 해제
	Scene.GetRegistry().Remove<FAudioSourceComponent>(Speaker);
	System.Update(Scene, Engine, Clip.parent_path());
	E_EXPECT_EQ(System.GetSourceCount(), 0u);
	E_EXPECT_EQ(Engine.GetSoundCount(), 0u);
}

E_TEST(AudioSystem_PlayOnStartFalseWaits)
{
	FAudioEngine Engine;
	E_EXPECT_TRUE(Engine.Init(MakeTestDesc()));
	const std::filesystem::path Clip = WriteSineWav("Loop.wav", 220.0f, 0.25f);

	FScene  Scene;
	FEntity Speaker = Scene.CreateEntity("Speaker");
	FAudioSourceComponent& Source = Scene.GetRegistry().Emplace<FAudioSourceComponent>(Speaker);
	Source.ClipAsset    = Clip.filename().string();
	Source.bPlayOnStart = false;
	Scene.UpdateTransforms();

	FAudioSystem System;
	System.Update(Scene, Engine, Clip.parent_path());
	E_EXPECT_FALSE(Engine.IsPlaying(System.GetSound(Speaker)));
	System.Play(Engine, Speaker);
	E_EXPECT_TRUE(Engine.IsPlaying(System.GetSound(Speaker)));

	System.Reset(Engine);
	E_EXPECT_EQ(Engine.GetSoundCount(), 0u);
}

E_TEST(AudioReflection_RegistersSourceComponent)
{
	RegisterAudioTypes();
	RegisterAudioTypes(); // 중복 호출 안전
	const FTypeInfo* Info = FTypeRegistry::Get().Find("AudioSourceComponent");
	E_EXPECT_TRUE(Info != nullptr);
	if (Info)
	{
		E_EXPECT_EQ(Info->Properties.size(), static_cast<size_t>(8));
	}
}

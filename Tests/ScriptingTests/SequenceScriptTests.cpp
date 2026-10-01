#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Sequence.h"
#include "Scene/SequencePlayer.h"
#include "Scripting/ScriptSystem.h"
#include "World/GameWorld.h"

#include <filesystem>
#include <fstream>

// Lua 시퀀스 (Phase 35-2): OnStart에서 entity:PlaySequence → 게임 월드 틱이 재생, 이벤트 → OnSequenceEvent_<이름>, 끝 → OnSequenceFinished
E_TEST(SequenceScript_PlayEventsAndFinish)
{
	const std::filesystem::path Content = FTestRegistry::GetTempDirectory() / L"ProjectESequenceScript";
	std::filesystem::create_directories(Content / L"Scripts");
	FSequenceAsset Asset;
	Asset.Duration = 0.5f;
	FSequenceTrack Door;
	Door.Type   = ESequenceTrackType::Transform;
	Door.Target = "Door";
	FSequenceTransformKey Start;
	Start.Interp           = ESequenceInterp::Linear;
	FSequenceTransformKey End = Start;
	End.Time               = 0.5f;
	End.Position           = FVector3(0.0f, 0.0f, 200.0f);
	Door.TransformKeys     = { Start, End };
	Asset.Tracks.push_back(Door);
	FSequenceTrack Events;
	Events.Type   = ESequenceTrackType::Event;
	Events.Events = { { 0.25f, "Halfway" } };
	Asset.Tracks.push_back(Events);
	const std::filesystem::path SequencePath = Content / L"Open.esequence";
	E_EXPECT_TRUE(Asset.SaveToFile(SequencePath));
	FSequenceLibrary::Get().Invalidate();
	{
		std::ofstream File(Content / L"Scripts/Director.lua", std::ios::binary | std::ios::trunc);
		File << R"(
local T = { Properties = { Sequence = "", Halfway = 0, Finished = 0, PlayingAtStart = false, Duration = 0 } }
function T:OnStart()
	assert(self.entity:PlaySequence(self.Properties.Sequence))
	self.Properties.PlayingAtStart = self.entity:IsSequencePlaying()
	self.Properties.Duration = self.entity:GetSequenceDuration()
end
function T:OnSequenceEvent_Halfway() self.Properties.Halfway = self.Properties.Halfway + 1 end
function T:OnSequenceFinished() self.Properties.Finished = self.Properties.Finished + 1 end
return T
)";
	}
	FScene        Scene;
	const FEntity DoorEntity = Scene.CreateEntity("Door");
	const FEntity Director   = Scene.CreateEntity("Director");
	FScriptComponent& Script = Scene.GetRegistry().Emplace<FScriptComponent>(Director);
	Script.ScriptAsset       = "Scripts/Director.lua";
	Script.PropertyOverrides = "{\"Sequence\": \"" + SequencePath.generic_string() + "\"}";

	FScriptSystem Scripts;
	FGameWorld    World;
	World.Init({ &Scripts, nullptr, nullptr, nullptr, Content });
	World.BeginPlay(Scene);
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		World.TickGameplay(1.0f / 60.0f, nullptr);
	}
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_TRUE(Scripts.GetInstanceProperty(Director, "PlayingAtStart").bBool);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Director, "Duration").Number, 0.5, 1.0e-6);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Director, "Halfway").Number, 1.0, 1.0e-9);
	E_EXPECT_NEAR(Scripts.GetInstanceProperty(Director, "Finished").Number, 1.0, 1.0e-9);
	E_EXPECT_NEAR(Scene.GetTransform(DoorEntity).Position.Z, 200.0f, 1.0e-3f);
	E_EXPECT_FALSE(FSequenceSystem::IsPlaying(Scene, Director));
	E_EXPECT_TRUE(Scene.GetRegistry().Has<FSequencePlayerComponent>(Director)); // PlaySequence가 붙였다
	World.EndPlay();
	std::error_code ErrorCode;
	std::filesystem::remove_all(Content, ErrorCode);
}

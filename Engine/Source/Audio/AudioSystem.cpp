#include "Audio/AudioSystem.h"

#include "Audio/AudioComponents.h"
#include "Core/StringConv.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <vector>

namespace
{
	FSoundDesc MakeSoundDesc(const FAudioSourceComponent& Source)
	{
		FSoundDesc Desc;
		Desc.Volume      = Source.Volume;
		Desc.Pitch       = Source.Pitch;
		Desc.bLoop       = Source.bLoop;
		Desc.bSpatial    = Source.bSpatial;
		Desc.MinDistance = Source.MinDistance;
		Desc.MaxDistance = Source.MaxDistance;
		return Desc;
	}
}

void FAudioSystem::Update(FScene& Scene, FAudioEngine& Engine, const std::filesystem::path& ContentDirectory)
{
	++FrameCounter;
	Engine.Update();
	if (!Engine.IsInitialized())
	{
		return;
	}

	Scene.GetRegistry().View<FTransformComponent, FAudioSourceComponent>().Each(
		[&](FEntity Entity, FTransformComponent& Transform, FAudioSourceComponent& Source) {
			FSourceState& State = Sources[Entity];
			const bool    bNew  = State.LastSeenFrame == 0;
			State.LastSeenFrame = FrameCounter;

			if (bNew || State.ClipAsset != Source.ClipAsset)
			{
				Engine.DestroySound(State.Sound);
				State.Sound     = {};
				State.ClipAsset = Source.ClipAsset;
				if (!Source.ClipAsset.empty())
				{
					State.Sound = Engine.CreateSound(ContentDirectory / FStringConv::ToWide(Source.ClipAsset), MakeSoundDesc(Source));
					Engine.SetWorldPosition(State.Sound, Transform.GetWorldPosition());
					if (Source.bPlayOnStart)
					{
						Engine.Play(State.Sound);
					}
				}
				return;
			}

			Engine.ApplyDesc(State.Sound, MakeSoundDesc(Source));
			Engine.SetWorldPosition(State.Sound, Transform.GetWorldPosition());
		});

	// 이번 프레임에 보이지 않은 소스(엔티티 삭제/컴포넌트 제거) 해제
	for (auto It = Sources.begin(); It != Sources.end();)
	{
		if (It->second.LastSeenFrame != FrameCounter)
		{
			Engine.DestroySound(It->second.Sound);
			It = Sources.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FAudioSystem::Reset(FAudioEngine& Engine)
{
	for (auto& [Entity, State] : Sources)
	{
		Engine.DestroySound(State.Sound);
	}
	Sources.clear();
}

void FAudioSystem::Play(FAudioEngine& Engine, FEntity Entity)
{
	Engine.Play(GetSound(Entity));
}

void FAudioSystem::Stop(FAudioEngine& Engine, FEntity Entity)
{
	Engine.Stop(GetSound(Entity));
}

FSoundHandle FAudioSystem::GetSound(FEntity Entity) const
{
	const auto It = Sources.find(Entity);
	return It != Sources.end() ? It->second.Sound : FSoundHandle{};
}

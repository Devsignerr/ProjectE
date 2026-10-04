#include "Scene/Sprite/FlipbookSystem.h"

#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <algorithm>

namespace
{
	// 경로/세대가 바뀌었으면 다시 읽는다. 반환: 재생 가능한 에셋이 있는가
	bool ResolveFlipbook(FFlipbookComponent& Component)
	{
		FFlipbookRuntime& Runtime    = Component.Runtime;
		FSprite2DLibrary& Library    = FSprite2DLibrary::Get();
		const bool        bPathMoved = Runtime.LoadedPath != Component.Flipbook;
		if (bPathMoved || Runtime.Generation != Library.GetGeneration())
		{
			Runtime.Asset      = Library.LoadFlipbook(Component.Flipbook);
			Runtime.LoadedPath = Component.Flipbook;
			Runtime.Generation = Library.GetGeneration();
			Runtime.Atlas.reset();
			Runtime.AtlasPath.clear();
			Runtime.FrameSlices.clear();
			if (Runtime.Asset != nullptr)
			{
				Runtime.AtlasPath = FSprite2DLibrary::ResolveReference(Component.Flipbook, Runtime.Asset->Sprite);
				Runtime.Atlas     = Library.LoadSprite(Runtime.AtlasPath);
				Runtime.FrameSlices.reserve(Runtime.Asset->Frames.size());
				for (const FFlipbookFrame& Frame : Runtime.Asset->Frames)
				{
					Runtime.FrameSlices.push_back(Runtime.Atlas != nullptr ? Runtime.Atlas->FindSlice(Frame.Slice) : -1);
				}
			}
			if (bPathMoved)
			{
				// 다른 플립북: 처음부터 (세대만 바뀐 핫 리로드는 재생 위치 유지)
				Runtime.bStarted  = false;
				Runtime.bFinished = false;
				Runtime.Frame     = -1;
			}
		}
		return Runtime.Asset != nullptr && !Runtime.Asset->FrameDurations.empty();
	}

	void WriteSpriteFrame(FRegistry& Registry, FEntity Entity, const FFlipbookRuntime& Runtime, bool bValid)
	{
		FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Entity);
		if (Sprite == nullptr)
		{
			return;
		}
		if (!bValid || Runtime.Atlas == nullptr || Runtime.Frame < 0 || static_cast<size_t>(Runtime.Frame) >= Runtime.FrameSlices.size())
		{
			Sprite->Runtime.FlipbookAtlas.reset();
			Sprite->Runtime.FlipbookAtlasPath.clear();
			Sprite->Runtime.FlipbookSliceIndex = -1;
			return;
		}
		Sprite->Runtime.FlipbookAtlas      = Runtime.Atlas;
		Sprite->Runtime.FlipbookAtlasPath  = Runtime.AtlasPath;
		Sprite->Runtime.FlipbookSliceIndex = Runtime.FrameSlices[static_cast<size_t>(Runtime.Frame)];
	}
} // namespace

void FFlipbookSystem::Update(FScene& Scene, float DeltaSeconds)
{
	FRegistry&         Registry = Scene.GetRegistry();
	std::vector<int32> EventIndices;
	Registry.View<FFlipbookComponent>().Each([&](FEntity Entity, FFlipbookComponent& Component) {
		FFlipbookRuntime& Runtime = Component.Runtime;
		Runtime.PendingEvents.clear();
		Runtime.bFinishedThisUpdate = false;
		const bool bValid = ResolveFlipbook(Component);
		if (!bValid)
		{
			Runtime.Frame = -1;
			WriteSpriteFrame(Registry, Entity, Runtime, false);
			return;
		}
		const FFlipbookAsset&  Asset     = *Runtime.Asset;
		const std::span<const float> Durations(Asset.FrameDurations);
		const bool             bOnce     = Asset.Loop == EFlipbookLoopMode::Once;
		const bool             bFirst    = !Runtime.bStarted;
		if (bFirst)
		{
			Runtime.Time      = Component.StartTime;
			Runtime.bStarted  = true;
			Runtime.bFinished = false;
		}
		const float Prev    = Runtime.Time;
		const bool  bAdvance = Component.bPlaying && !Runtime.bFinished && DeltaSeconds > 0.0f && Component.Speed != 0.0f;
		float       New     = bAdvance ? Prev + DeltaSeconds * Component.Speed : Prev;

		EventIndices.clear();
		if (bFirst || New != Prev)
		{
			FlipbookMath::CollectEvents(Prev, New, Durations, Asset.Loop, Asset.Events, bFirst, EventIndices);
		}
		if (bOnce && bAdvance)
		{
			const bool bForwardEnd  = Component.Speed > 0.0f && New >= Asset.TotalDuration;
			const bool bBackwardEnd = Component.Speed < 0.0f && New <= 0.0f;
			if (bForwardEnd || bBackwardEnd)
			{
				Runtime.bFinished           = true;
				Runtime.bFinishedThisUpdate = true;
				New               = bForwardEnd ? Asset.TotalDuration : 0.0f;
			}
		}
		Runtime.Time  = FlipbookMath::WrapTime(New, Durations, Asset.Loop);
		Runtime.Frame = FlipbookMath::Evaluate(Runtime.Time, Durations, Asset.Loop).Frame;
		for (const int32 Index : EventIndices)
		{
			const FFlipbookEvent& Event = Asset.Events[static_cast<size_t>(Index)];
			Runtime.PendingEvents.push_back({ Event.Name, Event.Frame });
		}
		WriteSpriteFrame(Registry, Entity, Runtime, true);
	});
}

bool FFlipbookSystem::Restart(FScene& Scene, FEntity Entity)
{
	FFlipbookComponent* Component = Scene.GetRegistry().TryGet<FFlipbookComponent>(Entity);
	if (Component == nullptr)
	{
		return false;
	}
	Component->Runtime.bStarted  = false;
	Component->Runtime.bFinished = false;
	return true;
}

bool FFlipbookSystem::SetTime(FScene& Scene, FEntity Entity, float Seconds)
{
	FFlipbookComponent* Component = Scene.GetRegistry().TryGet<FFlipbookComponent>(Entity);
	if (Component == nullptr)
	{
		return false;
	}
	FFlipbookRuntime& Runtime = Component->Runtime;
	Runtime.bStarted          = true; // 시작 프레임 이벤트를 내지 않는다 (스크럽)
	Runtime.Time              = Seconds;
	Runtime.bFinished         = false;
	if (ResolveFlipbook(*Component))
	{
		const FFlipbookAsset& Asset = *Runtime.Asset;
		if (Asset.Loop == EFlipbookLoopMode::Once)
		{
			Runtime.Time = std::clamp(Seconds, 0.0f, Asset.TotalDuration);
		}
		Runtime.Time  = FlipbookMath::WrapTime(Runtime.Time, Asset.FrameDurations, Asset.Loop);
		Runtime.Frame = FlipbookMath::Evaluate(Runtime.Time, Asset.FrameDurations, Asset.Loop).Frame;
		WriteSpriteFrame(Scene.GetRegistry(), Entity, Runtime, true);
	}
	return true;
}

int32 FFlipbookSystem::GetFrame(FScene& Scene, FEntity Entity)
{
	const FFlipbookComponent* Component = Scene.GetRegistry().TryGet<FFlipbookComponent>(Entity);
	return Component != nullptr ? Component->Runtime.Frame : -1;
}

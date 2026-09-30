#include "Editor/PlayMode.h"

#include "Core/Log.h"
#include "Editor/EditorContext.h"
#include "Renderer/SceneCamera.h"
#include "World/GameWorld.h"

E_DECLARE_LOG_CATEGORY(LogEditor)

void FPlayMode::Init(FScene& InEditScene, FGameWorld& InWorld)
{
	EditScene = &InEditScene;
	World     = &InWorld;
}

void FPlayMode::Play(FEditorContext& Context)
{
	if (IsActive())
	{
		return;
	}

	EditSelection = Context.SelectedEntity;
	FSceneCloner::Clone(*EditScene, PlayScene, &EntityMap);
	PlayScene.UpdateTransforms();

	Context.Scene = &PlayScene;
	const auto Mapped = EntityMap.find(EditSelection.ToId());
	Context.Select(Mapped != EntityMap.end() ? Mapped->second : NullEntity);

	State          = EState::Playing;
	bStepRequested = false;
	SyncContextFlags(Context);

	World->BeginPlay(PlayScene);
	E_LOG(LogEditor, Display, "플레이 시작 (엔티티 {}개)", PlayScene.GetRegistry().GetAliveCount());
}

void FPlayMode::Stop(FEditorContext& Context)
{
	if (!IsActive())
	{
		return;
	}

	World->EndPlay();
	PlayScene.Clear();
	EntityMap.clear();

	Context.Scene = EditScene;
	Context.Select(EditScene->GetRegistry().IsValid(EditSelection) ? EditSelection : NullEntity);
	EditSelection = NullEntity;

	State = EState::Editing;
	SyncContextFlags(Context);
	E_LOG(LogEditor, Display, "플레이 정지 — 편집 씬 복원");
}

void FPlayMode::TogglePause()
{
	if (State == EState::Playing)
	{
		State = EState::Paused;
	}
	else if (State == EState::Paused)
	{
		State = EState::Playing;
	}
}

void FPlayMode::RequestStep()
{
	if (State == EState::Paused)
	{
		bStepRequested = true;
	}
}

bool FPlayMode::Tick(FEditorContext& Context, float DeltaSeconds, const FInput* Input)
{
	SyncContextFlags(Context);
	if (State == EState::Editing)
	{
		return false;
	}

	float StepDelta = DeltaSeconds;
	if (State == EState::Paused)
	{
		if (!bStepRequested)
		{
			return false;
		}
		bStepRequested = false;
		StepDelta      = StepSeconds;
	}

	World->TickGameplay(StepDelta, Input);
	return true;
}

FCamera* FPlayMode::UpdateGameCamera(float AspectRatio)
{
	if (!IsActive())
	{
		return nullptr;
	}
	const FEntity CameraEntity = FSceneCamera::FindPrimary(PlayScene);
	if (!CameraEntity.IsValid() || !FSceneCamera::ApplyToCamera(PlayScene, CameraEntity, AspectRatio, GameCamera))
	{
		return nullptr;
	}
	return &GameCamera;
}

void FPlayMode::SyncContextFlags(FEditorContext& Context) const
{
	Context.bPlaying = State != EState::Editing;
	Context.bPaused  = State == EState::Paused;
}

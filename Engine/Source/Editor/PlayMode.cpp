#include "Editor/PlayMode.h"

#include "Core/Log.h"
#include "Editor/EditorContext.h"
#include "Renderer/SceneAssetResolver.h"
#include "Renderer/SceneCamera.h"
#include "Scene/SceneSerializer.h"
#include "World/GameWorld.h"

E_DECLARE_LOG_CATEGORY(LogEditor)

void FPlayMode::Init(FScene& InEditScene, FGameWorld& InWorld)
{
	EditScene = &InEditScene;
	World     = &InWorld;
}

void FPlayMode::Play(FEditorContext& Context, const FPlayOptions& Options)
{
	if (IsActive())
	{
		return;
	}

	EditSelection = Context.SelectedEntity;
	if (Options.SceneJson != nullptr)
	{
		// 네트워크 플레이: 다른 프로세스와 같은 로드 경로 (복제 대신 JSON) → 에셋 핸들 해석
		EntityMap.clear();
		if (!FSceneSerializer::FromJsonString(PlayScene, *Options.SceneJson))
		{
			E_LOG(LogEditor, Error, "플레이 씬을 만들지 못했습니다 (씬 JSON 오류)");
			PlayScene.Clear();
			return;
		}
		if (Context.Resources != nullptr)
		{
			FSceneAssetResolver::Resolve(PlayScene, *Context.Resources, Context.ContentDirectory);
		}
	}
	else
	{
		FSceneCloner::Clone(*EditScene, PlayScene, &EntityMap);
	}
	PlayScene.UpdateTransforms();

	Context.Scene = &PlayScene;
	const auto Mapped = EntityMap.find(EditSelection.ToId());
	Context.Select(Mapped != EntityMap.end() ? Mapped->second : NullEntity);

	State          = EState::Playing;
	bStepRequested = false;
	SyncContextFlags(Context);

	if (Options.BeforeBeginPlay)
	{
		Options.BeforeBeginPlay(PlayScene);
	}
	World->BeginPlay(PlayScene, Options.Mode);
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

#include "Editor/PlayMode.h"

#include "Core/Log.h"
#include "Editor/EditorContext.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/SceneAssetResolver.h"
#include "Renderer/SceneCamera.h"
#include "Scripting/ScriptSystem.h"

E_DECLARE_LOG_CATEGORY(LogEditor)

void FPlayMode::Init(FScene& InEditScene, FScriptSystem& InScripts, FPhysicsSystem* InPhysics)
{
	EditScene = &InEditScene;
	Scripts   = &InScripts;
	Physics   = InPhysics;
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

	if (Physics != nullptr)
	{
		Physics->Begin();
	}
	Scripts->BeginPlay(PlayScene);
	E_LOG(LogEditor, Display, "플레이 시작 (엔티티 {}개)", PlayScene.GetRegistry().GetAliveCount());
}

void FPlayMode::Stop(FEditorContext& Context)
{
	if (!IsActive())
	{
		return;
	}

	Scripts->EndPlay();
	if (Physics != nullptr)
	{
		Physics->End();
	}
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

	Scripts->Update(StepDelta, Input);
	if (Scripts->ConsumeSceneStructureChanged() && Context.Resources != nullptr)
	{
		// 스크립트가 만든 엔티티의 에셋 참조(primitive:cube, .emat 등)를 핸들로 복원
		FSceneAssetResolver::Resolve(PlayScene, *Context.Resources, Context.ContentDirectory);
	}
	if (Physics != nullptr)
	{
		Physics->Update(PlayScene, StepDelta);
	}
	PlayScene.UpdateTransforms();
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

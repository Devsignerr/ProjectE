#pragma once

#include "Core/CoreTypes.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"

class FInput;
class FScriptSystem;
struct FEditorContext;

// 에디터 플레이 모드 (뷰포트 안에서 재생, 정지 시 원래 씬 복원).
//   Play:  편집 씬을 PlayScene으로 복제(FSceneCloner) → Context.Scene을 PlayScene으로 전환 → 스크립트 시작
//   Stop:  스크립트 종료(OnDestroy) → PlayScene 폐기 → Context.Scene을 편집 씬으로 되돌림 (선택 복원)
//   Pause/Step: 일시정지 중에는 게임 로직을 멈추고, Step은 한 프레임(1/60초)만 진행
// 플레이 중 편집(기즈모/인스펙터)은 PlayScene에만 적용되고 정지하면 사라진다 (Unity/UE와 동일).
class FPlayMode
{
public:
	enum class EState : uint8
	{
		Editing,
		Playing,
		Paused,
	};

	static constexpr float StepSeconds = 1.0f / 60.0f;

	// EditScene/Scripts는 FPlayMode보다 오래 산다 (비소유)
	void Init(FScene& InEditScene, FScriptSystem& InScripts);

	void Play(FEditorContext& Context);
	void Stop(FEditorContext& Context);
	void TogglePause();
	void RequestStep(); // 일시정지 중에만 의미 있음

	// 게임 로직 한 프레임 (스크립트). 실제로 진행했으면 true. Input은 nullptr 허용 (UI가 입력을 가져간 경우)
	bool Tick(FEditorContext& Context, float DeltaSeconds, const FInput* Input);

	// 주 카메라 컴포넌트가 있으면 GameCamera를 그 시점으로 갱신하고 반환, 없으면 nullptr (에디터 카메라 사용)
	FCamera* UpdateGameCamera(float AspectRatio);

	EState GetState() const { return State; }
	bool   IsActive() const { return State != EState::Editing; }
	bool   IsPaused() const { return State == EState::Paused; }

	FScene& GetPlayScene() { return PlayScene; }

private:
	void SyncContextFlags(FEditorContext& Context) const;

	FScene*        EditScene = nullptr;
	FScriptSystem* Scripts   = nullptr;

	FScene                   PlayScene;
	FSceneCloner::FEntityMap EntityMap;      // 편집 씬 → 플레이 씬
	FEntity                  EditSelection;  // 재생 시작 시 편집 씬 선택 (정지 시 복원)
	FCamera                  GameCamera;
	EState                   State         = EState::Editing;
	bool                     bStepRequested = false;
};

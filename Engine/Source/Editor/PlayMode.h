#pragma once

#include "Core/CoreTypes.h"
#include "Network/NetTypes.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"
#include "Scene/SceneCloner.h"

#include <functional>
#include <string>

class FGameWorld;
class FInput;
struct FEditorContext;

// 플레이 시작 옵션 (기본 = 1인용: 편집 씬 복제)
struct FPlayOptions
{
	ENetMode Mode = ENetMode::Standalone;
	// 있으면 편집 씬을 복제하지 않고 이 씬 JSON을 로드한다 (네트워크 플레이: 모든 프로세스가 같은 JSON을 같은 방식으로
	// 로드해야 정적 NetId가 같다. 선택은 옮기지 않는다)
	const std::string* SceneJson = nullptr;
	// 월드 시작 직전 (정적 NetId 부여/복제 시작 등 — 스크립트가 엔티티를 만들기 전)
	std::function<void(FScene& PlayScene)> BeforeBeginPlay;
};

// 에디터 플레이 모드 (뷰포트 안에서 재생, 정지 시 원래 씬 복원).
//   Play:  편집 씬을 PlayScene으로 복제(FSceneCloner) → Context.Scene을 PlayScene으로 전환 → FGameWorld::BeginPlay
//   Stop:  FGameWorld::EndPlay(스크립트 OnDestroy, 물리 파괴) → PlayScene 폐기 → Context.Scene을 편집 씬으로 되돌림 (선택 복원)
//   Pause/Step: 일시정지 중에는 게임 로직을 멈추고, Step은 한 프레임(1/60초)만 진행
//   게임 로직 순서(스크립트/게임 모듈/물리)는 FGameWorld::TickGameplay가 정한다 (편집 모드에서는 돌지 않음)
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

	// EditScene/World는 FPlayMode보다 오래 산다 (비소유)
	void Init(FScene& InEditScene, FGameWorld& InWorld);

	void Play(FEditorContext& Context, const FPlayOptions& Options = {});
	void Stop(FEditorContext& Context);
	void TogglePause();
	void RequestStep(); // 일시정지 중에만 의미 있음

	// 게임 로직 한 프레임 (FGameWorld::TickGameplay). 실제로 진행했으면 true. Input은 nullptr 허용 (UI가 입력을 가져간 경우)
	bool Tick(FEditorContext& Context, float DeltaSeconds, const FInput* Input);

	// 주 카메라 컴포넌트가 있으면 GameCamera를 그 시점으로 갱신하고 반환, 없으면 nullptr (에디터 카메라 사용)
	FCamera* UpdateGameCamera(float AspectRatio);

	EState GetState() const { return State; }
	bool   IsActive() const { return State != EState::Editing; }
	bool   IsPaused() const { return State == EState::Paused; }

	FScene& GetPlayScene() { return PlayScene; }

private:
	void SyncContextFlags(FEditorContext& Context) const;

	FScene*     EditScene = nullptr;
	FGameWorld* World     = nullptr;

	FScene                   PlayScene;
	FSceneCloner::FEntityMap EntityMap;      // 편집 씬 → 플레이 씬
	FEntity                  EditSelection;  // 재생 시작 시 편집 씬 선택 (정지 시 복원)
	FCamera                  GameCamera;
	EState                   State         = EState::Editing;
	bool                     bStepRequested = false;
};

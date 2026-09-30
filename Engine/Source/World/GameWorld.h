#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>

class FGameModuleHost;
class FInput;
class FPhysicsSystem;
class FResourceManager;
class FScene;
class FScriptSystem;

// FGameWorld가 돌리는 시스템. 모두 비소유 — 앱이 소유하고 FGameWorld보다 오래 산다
struct FGameWorldSystems
{
	FScriptSystem*        Scripts    = nullptr; // 필수
	FPhysicsSystem*       Physics    = nullptr; // 없으면 물리 없음
	FGameModuleHost*      GameModule = nullptr; // 없으면 게임 모듈 없음
	FResourceManager*     Resources  = nullptr; // 없으면 에셋 핸들 해석을 건너뛴다 (GPU 없는 서버)
	std::filesystem::path ContentDirectory;
};

// 게임 월드 한 프레임의 갱신 순서. 런타임, 에디터 플레이 모드, (이후) 전용 서버가 같은 순서를 쓴다.
//   게임플레이 틱 (플레이 중에만): 스크립트 → 스크립트가 구조를 바꿨으면 에셋 해석 → 게임 모듈 → 물리 → UpdateTransforms
//   표시 틱 (편집 중에도):         애니메이션 → UpdateTransforms → 파티클 에셋 해석 → 파티클
// 시작/정지: BeginPlay = 물리 → 게임 모듈 → 스크립트, EndPlay = 역순
class FGameWorld
{
public:
	// 스크립트 물리 훅(Physics.Raycast, entity:AddForce 등)도 여기서 연결한다
	void Init(const FGameWorldSystems& InSystems);

	void BeginPlay(FScene& InScene);
	void EndPlay();
	bool IsPlaying() const { return Scene != nullptr; }

	// 게임플레이 한 프레임 (플레이 중이 아니면 무시). Input은 nullptr 허용 (UI가 입력을 가져간 경우)
	void TickGameplay(float DeltaSeconds, const FInput* Input);
	// 표시용 갱신. 플레이 여부와 무관하게 대상 씬을 갱신한다 (에디터는 편집 씬도)
	void TickPresentation(FScene& TargetScene, float DeltaSeconds);

	FScene*                  GetScene() const { return Scene; }
	const FGameWorldSystems& GetSystems() const { return Systems; }

private:
	FGameWorldSystems Systems;
	FScene*           Scene = nullptr; // 플레이 중인 씬 (비소유, BeginPlay~EndPlay)
};

#pragma once

#include "Core/CoreTypes.h"
#include "Network/NetTypes.h"

#include <filesystem>
#include <memory>

class FAISystem;
class FGameModuleHost;
class FInput;
class FNetDriver;
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
	FNetDriver*           Net        = nullptr; // 없으면 로컬 플레이어 ID 0 (Standalone)
};

// 월드를 돌리는 쪽 (넷 모드에서 정해진다). Authority = 서버/Standalone(게임 로직 전부), Client = 네트워크 클라이언트
// (게임 로직은 서버가 돌리고 복제로 받는다. ClientOnly/Both 스크립트와 물리만 돌리며, 복제 엔티티의 동적 바디는 키네마틱으로 복제 트랜스폼을 따른다)
enum class EWorldRole : uint8
{
	Authority,
	Client,
};

// 게임 월드 한 프레임의 갱신 순서. 런타임, 에디터 플레이 모드, (이후) 전용 서버가 같은 순서를 쓴다.
//   게임플레이 틱 (플레이 중에만): 스크립트 → 스크립트가 구조를 바꿨으면 에셋 해석 → 게임 모듈 → AI → 물리 → UpdateTransforms
//                                  (Client 역할: 게임 모듈·AI 없음 — 서버가 돌리고 복제로 받는다)
//   스크립트 ExecutionLocation 필터: Standalone/리슨 = 전부, 전용 서버 = ServerOnly/Both, 클라이언트 = ClientOnly/Both
//   표시 틱 (편집 중에도):         애니메이션 → UpdateTransforms → 파티클 에셋 해석 → 파티클
// 시작/정지: BeginPlay = 물리 → 게임 모듈 → AI → 스크립트 (Client 역할은 게임 모듈·AI 없음), EndPlay = 역순
// AI 시스템(비헤이비어 트리, 내비메시, 이동)은 FGameWorld가 소유한다 (앱마다 따로 둘 설정이 없다)
class FGameWorld
{
public:
	FGameWorld();
	~FGameWorld();
	FGameWorld(const FGameWorld&)            = delete;
	FGameWorld& operator=(const FGameWorld&) = delete;

	// 스크립트 물리 훅(Physics.Raycast, entity:AddForce 등)도 여기서 연결한다 (네트워크 훅은 BeginPlay에서)
	void Init(const FGameWorldSystems& InSystems);

	// Mode: 이 프로세스가 맡을 넷 모드 (네트워크 연결 전에 시작하는 서버도 있으므로 드라이버 상태가 아니라 앱이 정한다)
	void       BeginPlay(FScene& InScene, ENetMode InMode = ENetMode::Standalone);
	void       EndPlay();
	bool       IsPlaying() const { return Scene != nullptr; }
	EWorldRole GetRole() const { return Mode == ENetMode::Client ? EWorldRole::Client : EWorldRole::Authority; }
	ENetMode   GetMode() const { return Mode; }

	// 게임플레이 한 프레임 (플레이 중이 아니면 무시). Input은 nullptr 허용 (UI가 입력을 가져간 경우)
	void TickGameplay(float DeltaSeconds, const FInput* Input);
	// 표시용 갱신. 플레이 여부와 무관하게 대상 씬을 갱신한다 (에디터는 편집 씬도)
	void TickPresentation(FScene& TargetScene, float DeltaSeconds);

	FScene*                  GetScene() const { return Scene; }
	const FGameWorldSystems& GetSystems() const { return Systems; }
	// 트리 블랙보드/이동 요청/내비메시 지정 (스크립트, 에디터 디버그 표시). 항상 유효
	FAISystem& GetAI() { return *AI; }

private:
	FGameWorldSystems          Systems;
	std::unique_ptr<FAISystem> AI;
	FScene*           Scene = nullptr; // 플레이 중인 씬 (비소유, BeginPlay~EndPlay)
	ENetMode          Mode  = ENetMode::Standalone;
};

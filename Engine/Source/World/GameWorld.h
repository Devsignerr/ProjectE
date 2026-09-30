#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Input.h"
#include "Network/NetTypes.h"
#include "Scene/GameRpc.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FAISystem;
class FGameModuleHost;
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

// 게임 월드 한 프레임의 갱신 순서. 런타임, 에디터 플레이 모드, 전용 서버가 같은 순서를 쓴다.
//   게임플레이 틱 (플레이 중에만): 스크립트 → 스크립트가 구조를 바꿨으면 에셋 해석 → 게임 모듈 → AI → 물리 → UpdateTransforms
//                                  (Client 역할: 게임 모듈·AI 없음 — 서버가 돌리고 복제로 받는다)
//   스크립트 ExecutionLocation 필터: Standalone/리슨 = 전부, 전용 서버 = ServerOnly/Both, 클라이언트 = ClientOnly/Both
//   입력: 클라이언트는 게임플레이 틱마다 로컬 입력 상태를 서버로 보낸다(비신뢰). 서버 스크립트의 Lua Input은
//         엔티티 소유 플레이어의 입력 (서버 소유/호스트 소유는 로컬 입력, 전용 서버의 서버 소유는 입력 없음)
//   표시 틱 (편집 중에도):         애니메이션 → UpdateTransforms → 파티클 에셋 해석 → 파티클
// 시작/정지: BeginPlay = 물리 → 게임 모듈 → 스크립트(Lua 상태) → AI (Client 역할은 게임 모듈·AI 없음), EndPlay = 역순.
//   스크립트 OnStart는 첫 게임플레이 틱에 불리므로 AI(트리 시작)가 스크립트 뒤여도 OnStart가 블랙보드를 쓰기 전에 트리가 있다
// RPC(스크립트/게임 모듈 공용)와 입력·플레이어 이벤트는 GameWorldNet.cpp. 게임 모듈에는 IGameNet으로 자신을 넘긴다
// AI 시스템(비헤이비어 트리, 내비메시, 이동)은 FGameWorld가 소유한다 (앱마다 따로 둘 설정이 없다)
class FGameWorld final : public IGameNet
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

	// FNetDriver::OnGameMessage에서 받은 월드 메시지(RPC, 입력). 처리했으면 true (복제 메시지는 복제 객체가 먼저 받는다)
	bool HandleNetMessage(FNetConnectionId Connection, const std::vector<uint8>& Message);

	// 서버: 플레이어 입장/퇴장 → 스크립트 OnPlayerJoined(playerId, pawn)/OnPlayerLeft(playerId) + 게임 모듈
	void OnPlayerJoined(uint32 PlayerId, FEntity Pawn);
	void OnPlayerLeft(uint32 PlayerId);

	FScene*                  GetScene() const { return Scene; }
	const FGameWorldSystems& GetSystems() const { return Systems; }
	// 트리 블랙보드/이동 요청/내비메시 지정 (스크립트, 에디터 디버그 표시). 항상 유효
	FAISystem& GetAI() { return *AI; }

	// ---- IGameNet (게임 모듈용)
	bool  IsServer() const override { return Mode != ENetMode::Client; }
	bool  IsClient() const override { return Mode != ENetMode::DedicatedServer; }
	int32 GetLocalPlayerId() const override;
	int32 GetOwner(FEntity Entity) const override;
	void  CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args) override;

private:
	// 잘못된 호출(클라이언트에서 Client/Multicast 등)은 std::runtime_error (Lua에서는 스크립트 오류가 된다)
	void          RouteRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args);
	void          InvokeRpcLocally(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args);
	void          ReceiveRpc(FNetConnectionId Connection, const std::vector<uint8>& Message);
	const FInput* ResolveInput(FEntity Entity, const FInput* LocalInput) const;
	void          SendLocalInput(const FInput& Input);
	void          ReceivePlayerInput(FNetConnectionId Connection, const std::vector<uint8>& Message);
	void          InstallScriptNetHooks();
	// 스크립트 ↔ AI 훅 연결 (Lua 블랙보드/이동 API, Lua 비헤이비어 트리 노드)
	void          ConnectScriptsAndAI();

	struct FRemoteInput
	{
		FInput Input;
		uint32 LastSequence = 0;
	};

	FGameWorldSystems          Systems;
	std::unique_ptr<FAISystem> AI;
	FScene*           Scene = nullptr; // 플레이 중인 씬 (비소유, BeginPlay~EndPlay)
	ENetMode          Mode  = ENetMode::Standalone;

	std::unordered_map<uint32, FRemoteInput> RemoteInputs;      // 서버: 플레이어 ID → 받은 입력
	uint32                                   InputSequence = 0; // 클라이언트: 보낸 입력 순번
};

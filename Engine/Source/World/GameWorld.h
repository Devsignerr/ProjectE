#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Input.h"
#include "Network/LanDiscovery.h"
#include "Network/NetTypes.h"
#include "Physics/CharacterMovement.h"
#include "Scene/GameRpc.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
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

// 스크립트(Net.Host/Connect/Disconnect)가 요청한 세션 전환. 앱이 ConsumeSessionRequest로 꺼내 처리한다
struct FNetSessionRequest
{
	enum class EType : uint8
	{
		Host,       // 리슨 서버로 (Standalone이면 재시작 없이)
		Connect,    // Address 서버에 클라이언트로 (씬을 다시 연다)
		Disconnect, // Standalone으로 (씬을 다시 연다)
	};
	EType       Type = EType::Disconnect;
	std::string Address;
	uint16      Port = 0; // Host: 0이면 기본 포트
};

// 월드를 돌리는 쪽 (넷 모드에서 정해진다). Authority = 서버/Standalone(게임 로직 전부), Client = 네트워크 클라이언트
// (게임 로직은 서버가 돌리고 복제로 받는다. ClientOnly/Both 스크립트와 물리만 돌리며, 복제 엔티티의 동적 바디는 키네마틱으로 복제 트랜스폼을 따른다)
enum class EWorldRole : uint8
{
	Authority,
	Client,
};

// 게임 월드 한 프레임의 갱신 순서. 런타임, 에디터 플레이 모드, 전용 서버가 같은 순서를 쓴다.
//   게임플레이 틱 (플레이 중에만): 스크립트 → 스크립트가 구조를 바꿨으면 에셋 해석 → 게임 모듈 → AI → 게임플레이 규칙 → 물리 → UpdateTransforms
//                                  게임플레이 규칙 (GameWorldGameplay.cpp, 서버): 데미지 이벤트 → OnDamaged/OnDeath, 사망 처리(점수/파괴/리스폰 예약),
//                                  리스폰, 매치 진행. 모든 역할: 매치 상태가 바뀌면 스크립트 OnMatchStateChanged(state)
//                                  (Client 역할: 게임 모듈·AI 없음 — 서버가 돌리고 복제로 받는다)
//   스크립트 ExecutionLocation 필터: Standalone/리슨 = 전부, 전용 서버 = ServerOnly/Both, 클라이언트 = ClientOnly/Both
//   입력: 클라이언트는 게임플레이 틱마다 로컬 입력 상태를 서버로 보낸다(비신뢰). 서버 스크립트의 Lua Input은
//         엔티티 소유 플레이어의 입력 (서버 소유/호스트 소유는 로컬 입력, 전용 서버의 서버 소유는 입력 없음).
//         입력 액션 값(Input.GetAction)도 함께 보낸다 — 클라이언트가 자기 바인딩으로 계산한 값. 게임 모듈은 IGameNet::GetInput
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
	// 네트워크 드라이버를 나중에 연결/해제 (에디터 플레이: 네트워크 플레이를 시작할 때만 드라이버가 생긴다). BeginPlay 전에
	void SetNetDriver(FNetDriver* InNet) { Systems.Net = InNet; }
	// 표시용 갱신. 플레이 여부와 무관하게 대상 씬을 갱신한다 (에디터는 편집 씬도)
	void TickPresentation(FScene& TargetScene, float DeltaSeconds);

	// FNetDriver::OnGameMessage에서 받은 월드 메시지(RPC, 입력). 처리했으면 true (복제 메시지는 복제 객체가 먼저 받는다)
	bool HandleNetMessage(FNetConnectionId Connection, const std::vector<uint8>& Message);

	// 서버: 플레이어 입장/퇴장 → 스크립트 OnPlayerJoined(playerId, pawn)/OnPlayerLeft(playerId) + 게임 모듈.
	// 이미 돌고 있는 스크립트만 받는다 (방금 생성된 폰의 스크립트는 다음 게임플레이 틱에 OnStart)
	void OnPlayerJoined(uint32 PlayerId, FEntity Pawn);
	void OnPlayerLeft(uint32 PlayerId);

	// 세션 전환 (로비): 스크립트 요청 꺼내기, 플레이 중 넷 모드 바꾸기 (Standalone ↔ 리슨 서버처럼 스크립트 필터가 같은 경우만)
	std::optional<FNetSessionRequest> ConsumeSessionRequest();
	void                              SetNetMode(ENetMode InMode);
	void                              SetLanDiscoveryPort(uint16 Port) { LanDiscoveryPort = Port; } // 테스트용

	FScene*                  GetScene() const { return Scene; }
	const FGameWorldSystems& GetSystems() const { return Systems; }
	// 트리 블랙보드/이동 요청/내비메시 지정 (스크립트, 에디터 디버그 표시). 항상 유효
	FAISystem& GetAI() { return *AI; }

	// ---- IGameNet (게임 모듈용)
	bool  IsServer() const override { return Mode != ENetMode::Client; }
	bool  IsClient() const override { return Mode != ENetMode::DedicatedServer; }
	int32 GetLocalPlayerId() const override;

	// 소유 클라이언트가 예측하는 캐릭터인가 (복제 클라이언트는 이 엔티티의 스냅샷 트랜스폼을 쓰지 않는다)
	bool   IsPredicted(FEntity Entity) const;
	// 예측 옵션: 캐릭터 이동 컴포넌트 bClientPrediction && 프로젝트 설정 네트워크 → 클라이언트 예측
	bool   UsesClientPrediction(FEntity Entity) const;
	uint32 GetCharacterCorrectionCount() const { return CharacterCorrections; }
	int32 GetOwner(FEntity Entity) const override;
	void  CallRpc(FEntity Target, EGameRpcKind Kind, const std::string& Name, const FGameRpcArgs& Args) override;
	const FInput* GetInput(FEntity Entity) const override { return ResolveInput(Entity, TickLocalInput); }

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
		FInput   Input;
		FVector2 ControlRotation; // yaw, pitch (도)
		uint32   LastSequence = 0;
		bool     bWarnedActionLayout = false; // 액션 목록 불일치 경고는 한 번만
	};
	const FInput* TickLocalInput = nullptr; // TickGameplay 동안만: 앱이 넘긴 로컬 입력 (게임 모듈 GetInput)
	FVector2 LocalControlRotation; // 로컬 플레이어 (Lua Net.SetControlRotation) — 클라이언트는 입력과 함께 보낸다

	struct FPredictedCharacter // 클라이언트: 서버가 아직 확인하지 않은 내 무브
	{
		std::deque<FCharacterMove> Moves;
		uint32                     NextSequence    = 0;
		uint32                     LastAckSequence = 0;
		FVector3                   VisualOffset; // 보정으로 생긴 위치 차이를 화면에서만 천천히 흡수 (시뮬레이션은 즉시 보정)
	};
	struct FServerCharacter // 서버: 원격 플레이어 캐릭터
	{
		std::vector<FCharacterMove> Queue;            // 받았지만 아직 적용하지 않은 무브 (순번 순)
		uint32                      LastQueued   = 0; // 큐에 넣은 마지막 순번 (겹쳐 온 무브 거르기)
		uint32                      LastApplied  = 0;
	};
	std::unordered_map<FEntity, FPredictedCharacter> PredictedCharacters;
	std::unordered_map<FEntity, FServerCharacter>    ServerCharacters;
	uint32                                           CharacterCorrections = 0; // 재조정에서 1cm 넘게 고친 횟수 (테스트/통계)
	FVector2 GetControlRotation(FEntity Entity) const;

	// 캐릭터 이동 (FCharacterMovementComponent): 조종하는 쪽은 입력으로 시뮬레이션, 아니면 복제 트랜스폼을 따라간다
	// 클라이언트 예측 (World/GameWorldCharacter.cpp 머리 주석): 소유 클라이언트는 무브를 바로 적용·기록·전송하고, 서버 ack로 재조정한다
	void TickCharacters(float DeltaSeconds);
	void SendCharacterMoves(FEntity Entity);
	void ReceiveCharacterMoves(FNetConnectionId Connection, const std::vector<uint8>& Message);
	void ReceiveCharacterAck(const std::vector<uint8>& Message);
	void SendCharacterAck(FEntity Entity, uint32 Sequence);
	bool IsLocallyControlled(FEntity Entity) const; // 이 프로세스가 조종: 소유 플레이어가 로컬이거나, 서버 소유(owner < 0)를 서버/Standalone이

	// 게임플레이 규칙 (Scene/Gameplay.h 체력·게임 모드, World/GameWorldGameplay.cpp)
	void  TickGameplayRules(float DeltaSeconds);
	void  DispatchDamageEvents();
	void  HandleDeath(FEntity Victim, FEntity Instigator);
	void  Respawn(FEntity Entity);
	int32 GetScoringPlayer(FEntity Instigator, FEntity Victim) const; // 처치 점수를 받을 플레이어 (-1 = 없음)
	int32  LastMatchState    = -1; // OnMatchStateChanged 감지 (-1 = 게임 모드 없음/시작 전)
	uint32 RespawnStartIndex = 0;  // 리스폰 PlayerStart 순번

	FGameWorldSystems          Systems;
	std::unique_ptr<FAISystem> AI;
	FScene*           Scene = nullptr; // 플레이 중인 씬 (비소유, BeginPlay~EndPlay)
	ENetMode          Mode  = ENetMode::Standalone;

	std::unordered_map<uint32, FRemoteInput> RemoteInputs;      // 서버: 플레이어 ID → 받은 입력
	uint32                                   InputSequence = 0; // 클라이언트: 보낸 입력 순번

	FLanDiscovery                     SessionSearch; // Net.FindSessions
	uint16                            LanDiscoveryPort = 0; // 0 = 프로젝트 설정
	std::optional<FNetSessionRequest> PendingSessionRequest;
};
